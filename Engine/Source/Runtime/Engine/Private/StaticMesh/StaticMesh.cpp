#include "StaticMesh/StaticMesh.h"

#include "Asset/CookedMeshLoadManager.h"

#include "DObject/Package.h"

#include "Asset/Asset.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Hash/XxHash.h"
#include "Logging/LogMacros.h"
#include "Math/Operations.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshBuilder.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Threading/RunnableThread.h"

#include "RHI.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		auto CheckStaticMeshUpdateThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto InitializeStaticMeshCandidate(
			FStaticMeshRenderData& Candidate) -> std::expected<void, FStaticMeshBuildFailure>
		{
			if (GDynamicRHI == nullptr)
			{
				return {};
			}

			std::atomic<bool> bInitialized = false;
			ENQUEUE_RENDER_COMMAND(InitStaticMeshCandidateResources)(
				[&Candidate, &bInitialized](
					FRHICommandListImmediate& CommandList) {
					bInitialized.store(
						Candidate.InitResources(CommandList),
						std::memory_order_release);
				});
			FRenderCommandFence InitFence;
			InitFence.BeginFence();
			InitFence.Wait();
			if (!bInitialized.load(std::memory_order_acquire))
			{
				check(Candidate.GetNumInitializedResources() == 0);
				return std::unexpected(FStaticMeshBuildFailure{"Static-mesh candidate resource initialization failed.", EStaticMeshBuildStage::Resources});
			}
			return {};
		}

		auto RetireStaticMeshRenderData(
			std::unique_ptr<FStaticMeshRenderData>& RenderData) -> void
		{
			if (RenderData == nullptr) return;
			if (GDynamicRHI == nullptr)
			{
				check(RenderData->GetNumInitializedResources() == 0);
				return;
			}

			FStaticMeshRenderData* RenderDataToRelease = RenderData.get();
			ENQUEUE_RENDER_COMMAND(ReleaseRetiredStaticMeshResources)(
				[RenderDataToRelease](FRHICommandListImmediate&) {
					RenderDataToRelease->ReleaseResources();
				});
			FRenderCommandFence ReleaseFence;
			ReleaseFence.BeginFence();
			ReleaseFence.Wait();
			check(RenderData->GetNumInitializedResources() == 0);
		}

		constexpr float VectorTolerance = 1.0e-10f;

		auto ImportAxisVector(EStaticMeshImportAxis Axis, FVector3f& OutVector, uint32& OutComponent) -> bool
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: OutVector = FVector3f(1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::NegativeX: OutVector = FVector3f(-1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::PositiveY: OutVector = FVector3f(0.0f, 1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::NegativeY: OutVector = FVector3f(0.0f, -1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::PositiveZ: OutVector = FVector3f(0.0f, 0.0f, 1.0f); OutComponent = 2; return true;
			case EStaticMeshImportAxis::NegativeZ: OutVector = FVector3f(0.0f, 0.0f, -1.0f); OutComponent = 2; return true;
			}
			return false;
		}

	}

	auto FormatStaticMeshImportSettingsError(const FStaticMeshImportSettingsError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshImportSettingsError::None: return {};
		case EStaticMeshImportSettingsError::UnknownAxis: return "The import coordinate system contains an unknown axis.";
		case EStaticMeshImportSettingsError::RepeatedAxis: return "Forward, Right, and Up must use X, Y, and Z exactly once.";
		}
		return {};
	}

	auto FStaticMeshImportSettings::Validate() const -> std::expected<void, FStaticMeshImportSettingsError>
	{
		FVector3f UnusedVector;
		uint32 ForwardComponent = 0;
		uint32 RightComponent = 0;
		uint32 UpComponent = 0;
		const bool bAxesKnown = ImportAxisVector(ForwardAxis, UnusedVector, ForwardComponent)
			&& ImportAxisVector(RightAxis, UnusedVector, RightComponent)
			&& ImportAxisVector(UpAxis, UnusedVector, UpComponent);
		if (!bAxesKnown)
			return std::unexpected(FStaticMeshImportSettingsError{.Code = EStaticMeshImportSettingsError::UnknownAxis,
				.ForwardAxis = ForwardAxis, .RightAxis = RightAxis, .UpAxis = UpAxis});
		if (ForwardComponent == RightComponent || ForwardComponent == UpComponent || RightComponent == UpComponent)
			return std::unexpected(FStaticMeshImportSettingsError{.Code = EStaticMeshImportSettingsError::RepeatedAxis,
				.ForwardAxis = ForwardAxis, .RightAxis = RightAxis, .UpAxis = UpAxis});
		return {};
	}

	auto FStaticMeshImportSettings::MakeDurin() -> FStaticMeshImportSettings
	{
		return {};
	}

	auto FStaticMeshImportSettings::MakeYUpNegativeZForward() -> FStaticMeshImportSettings
	{
		return {
			.ForwardAxis = EStaticMeshImportAxis::NegativeZ,
			.RightAxis = EStaticMeshImportAxis::PositiveX,
			.UpAxis = EStaticMeshImportAxis::PositiveY
		};
	}

	DStaticMesh::DStaticMesh(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	DStaticMesh::~DStaticMesh() = default;

	auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::RequestRenderDataAndResources() -> FCookedMeshLoadStatus
	{
		CheckStaticMeshUpdateThread();
		if (RenderData)
		{
			CookedLoadPhase.store(
				ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
			if (LoadRenderResourceState() == EStaticMeshRenderResourceState::Uninitialized)
				InitResources();
		}
		else
		{
			const ECookedMeshCpuPhase Phase =
				CookedLoadPhase.load(std::memory_order_acquire);
			if (Phase == ECookedMeshCpuPhase::Unloaded)
				SubmitCookedRenderDataRequest(true);
		}
		return GetRenderDataLoadStatus();
	}

	auto DStaticMesh::GetRenderDataLoadStatus() const -> FCookedMeshLoadStatus
	{
		const FStaticMeshRenderResourceStatus Resource = GetRenderResourceStatus();
		ECookedMeshGpuPhase GpuPhase = ECookedMeshGpuPhase::Unavailable;
		switch (Resource.Readiness)
		{
		case EStaticMeshRenderResourceReadiness::Queued: GpuPhase = ECookedMeshGpuPhase::Queued; break;
		case EStaticMeshRenderResourceReadiness::Ready: GpuPhase = ECookedMeshGpuPhase::Ready; break;
		case EStaticMeshRenderResourceReadiness::Failed: GpuPhase = ECookedMeshGpuPhase::Failed; break;
		case EStaticMeshRenderResourceReadiness::Unavailable: break;
		}
		return {.CpuPhase = RenderData ? ECookedMeshCpuPhase::CpuReady
			: CookedLoadPhase.load(std::memory_order_acquire),
			.GpuPhase = GpuPhase,
			.Generation = CookedLoadGeneration.load(std::memory_order_acquire),
			.ResourceRevision = Resource.Revision};
	}

	auto DStaticMesh::EnsureRenderDataLoadedBlocking()
		-> FCookedMeshBlockingResult
	{
		CheckStaticMeshUpdateThread();
		const ECookedMeshCpuPhase LoadPhase =
			CookedLoadPhase.load(std::memory_order_acquire);
		if (LoadPhase == ECookedMeshCpuPhase::Failed
			|| LoadPhase == ECookedMeshCpuPhase::Cancelled)
		{
			if (FCookedMeshLoadManager* Manager =
				GetCookedMeshLoadManager())
				Manager->Cancel(FObjectKey(this));
			CookedLoadError = {};
			CookedLoadPhase.store(ECookedMeshCpuPhase::Unloaded, std::memory_order_release);
			CookedLoadGeneration.fetch_add(1, std::memory_order_acq_rel);
		}
		if (!RenderData && CookedLoadPhase.load(std::memory_order_acquire)
			== ECookedMeshCpuPhase::Unloaded)
			SubmitCookedRenderDataRequest(false);
		const FCookedMeshLoadStatus Initial = GetRenderDataLoadStatus();
		if (!Initial.HasCpuData()
			&& Initial.CpuPhase != ECookedMeshCpuPhase::Failed)
		{
			if (FCookedMeshLoadManager* Manager =
				GetCookedMeshLoadManager();
				Manager && Initial.CpuPhase != ECookedMeshCpuPhase::Unloaded)
			{
				Manager->Finish(FObjectKey(this));
			}
		}
		if (!RenderData && CookedLoadPhase.load(std::memory_order_acquire)
			!= ECookedMeshCpuPhase::Failed
			&& GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedRenderData.GetMetadata().LogicalSize != 0)
		{
			CookedLoadPhase.store(ECookedMeshCpuPhase::Reading, std::memory_order_release);
			if (const auto Loaded = LoadCookedRenderData(); !Loaded)
			{
				CookedLoadPhase.store(ECookedMeshCpuPhase::Failed, std::memory_order_release);
				return {.Status = GetRenderDataLoadStatus(), .Error = Loaded.Error};
			}
			CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
		}
		if (RenderData)
		{
			CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
		}
		FCookedMeshBlockingResult Result{.Status = GetRenderDataLoadStatus()};
		if (!Result.Status.HasCpuData())
			Result.Error = CookedLoadError.Code != ECookedMeshLoadError::None ? CookedLoadError
				: FCookedMeshLoadError{.Code = ECookedMeshLoadError::Unavailable, .Owner = FObjectKey(this)};
		return Result;
	}

	auto DStaticMesh::GetRenderResourceStatus() const
		-> FStaticMeshRenderResourceStatus
	{
		const uint64 Packed =
			RenderResourceStatus.load(std::memory_order_acquire);
		EStaticMeshRenderResourceReadiness Readiness =
			EStaticMeshRenderResourceReadiness::Unavailable;
		switch (UnpackRenderResourceState(Packed))
		{
		case EStaticMeshRenderResourceState::InitializationQueued:
			Readiness = EStaticMeshRenderResourceReadiness::Queued;
			break;
		case EStaticMeshRenderResourceState::Ready:
			Readiness = EStaticMeshRenderResourceReadiness::Ready;
			break;
		case EStaticMeshRenderResourceState::Failed:
			Readiness = EStaticMeshRenderResourceReadiness::Failed;
			break;
		case EStaticMeshRenderResourceState::Uninitialized:
		case EStaticMeshRenderResourceState::ReleaseQueued:
		case EStaticMeshRenderResourceState::Released:
			break;
		}
		return {
			.Readiness = Readiness,
			.Revision = UnpackRenderResourceRevision(Packed)};
	}

	auto DStaticMesh::GetLOD0LocalBounds() const -> std::optional<FBox>
	{
		if (RenderData == nullptr || RenderData->LODResources.empty())
			return std::nullopt;
		const FBox& Bounds = RenderData->LODResources.front().LocalBounds;
		if (!Bounds.bIsValid
			|| !std::isfinite(Bounds.Min.x)
			|| !std::isfinite(Bounds.Min.y)
			|| !std::isfinite(Bounds.Min.z)
			|| !std::isfinite(Bounds.Max.x)
			|| !std::isfinite(Bounds.Max.y)
			|| !std::isfinite(Bounds.Max.z))
		{
			return std::nullopt;
		}
		return Bounds;
	}

	auto DStaticMesh::GetLOD0VolumetricBounds() const -> std::optional<FBox>
	{
		const std::optional<FBox> Bounds = GetLOD0LocalBounds();
		if (!Bounds) return std::nullopt;
		const FVector3 Size = Bounds->Max - Bounds->Min;
		if (Size.x <= 0.0 || Size.y <= 0.0 || Size.z <= 0.0)
			return std::nullopt;
		return Bounds;
	}

	auto DStaticMesh::LoadRenderResourceState() const
		-> EStaticMeshRenderResourceState
	{
		return UnpackRenderResourceState(
			RenderResourceStatus.load(std::memory_order_acquire));
	}

	auto DStaticMesh::PublishRenderResourceState(
		EStaticMeshRenderResourceState State) -> void
	{
		uint64 Current = RenderResourceStatus.load(std::memory_order_acquire);
		for (;;)
		{
			const uint64 Revision = UnpackRenderResourceRevision(Current);
			check(Revision < (std::numeric_limits<uint64>::max()
				>> RenderResourceStateBits));
			const uint64 Desired = PackRenderResourceStatus(State, Revision + 1);
			if (RenderResourceStatus.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel))
			{
				return;
			}
		}
	}

	auto DStaticMesh::TryPublishRenderResourceState(
		EStaticMeshRenderResourceState Expected,
		EStaticMeshRenderResourceState State) -> bool
	{
		uint64 Current = RenderResourceStatus.load(std::memory_order_acquire);
		for (;;)
		{
			if (UnpackRenderResourceState(Current) != Expected) return false;
			const uint64 Revision = UnpackRenderResourceRevision(Current);
			check(Revision < (std::numeric_limits<uint64>::max()
				>> RenderResourceStateBits));
			const uint64 Desired = PackRenderResourceStatus(State, Revision + 1);
			if (RenderResourceStatus.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel))
			{
				return true;
			}
		}
	}

	auto DStaticMesh::AdvanceRenderResourceRevision() -> void
	{
		PublishRenderResourceState(LoadRenderResourceState());
	}

	auto DStaticMesh::InitResources() -> void
	{
		CheckStaticMeshUpdateThread();
		if (RenderData == nullptr || GDynamicRHI == nullptr) return;

		const EStaticMeshRenderResourceState State = LoadRenderResourceState();
		if ((State != EStaticMeshRenderResourceState::Uninitialized
				&& State != EStaticMeshRenderResourceState::Failed)
			|| !TryPublishRenderResourceState(
				State, EStaticMeshRenderResourceState::InitializationQueued))
		{
			return;
		}

#if DURIN_BUILD_DEBUG
		RenderData->SetResourceDebugOwner(GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName())));
#endif
		FStaticMeshRenderData* RenderDataToInitialize = RenderData.get();
		ENQUEUE_RENDER_COMMAND(InitStaticMeshResources)(
			[this, RenderDataToInitialize](
				FRHICommandListImmediate& CommandList) {
				const EStaticMeshRenderResourceState Result =
					RenderDataToInitialize->InitResources(CommandList)
					? EStaticMeshRenderResourceState::Ready
					: EStaticMeshRenderResourceState::Failed;
				TryPublishRenderResourceState(
					EStaticMeshRenderResourceState::InitializationQueued,
					Result);
			});
	}

	auto DStaticMesh::ReleaseResources() -> void
	{
		CheckStaticMeshUpdateThread();
		const EStaticMeshRenderResourceState State =
			LoadRenderResourceState();
		if (State == EStaticMeshRenderResourceState::Released
			|| State == EStaticMeshRenderResourceState::ReleaseQueued)
		{
			return;
		}

		if (RenderData == nullptr
			|| State == EStaticMeshRenderResourceState::Uninitialized)
		{
			check(RenderData == nullptr
				|| RenderData->GetNumInitializedResources() == 0);
			PublishRenderResourceState(
				EStaticMeshRenderResourceState::Released);
			return;
		}

		PublishRenderResourceState(
			EStaticMeshRenderResourceState::ReleaseQueued);
		FStaticMeshRenderData* RenderDataToRelease = RenderData.get();
		ENQUEUE_RENDER_COMMAND(ReleaseStaticMeshResources)(
			[this, RenderDataToRelease](FRHICommandListImmediate&) {
				RenderDataToRelease->ReleaseResources();
				check(
					RenderDataToRelease->GetNumInitializedResources()
					== 0);
				PublishRenderResourceState(
					EStaticMeshRenderResourceState::Released);
			});
	}

	auto DStaticMesh::GetMaterialSlot(uint32 SlotIndex) const -> const FMeshMaterialSlotDefinition*
	{
		return SlotIndex < MaterialSlots.size() ? &MaterialSlots[SlotIndex] : nullptr;
	}

	auto DStaticMesh::FindMaterialSlot(FName Name) const -> const FMeshMaterialSlotDefinition*
	{
		const auto It = std::ranges::find(MaterialSlots, Name, &FMeshMaterialSlotDefinition::Name);
		return It == MaterialSlots.end() ? nullptr : &*It;
	}

	auto DStaticMesh::GetMaterialIndex(FName Name) const -> std::optional<uint32>
	{
		if (Name.IsNone()) return std::nullopt;
		const auto It = std::ranges::find(MaterialSlots, Name, &FMeshMaterialSlotDefinition::Name);
		if (It == MaterialSlots.end()) return std::nullopt;
		return static_cast<uint32>(std::distance(MaterialSlots.begin(), It));
	}

	auto FormatStaticMeshSlotRenameError(const FStaticMeshSlotRenameError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshSlotRenameError::None: return {};
		case EStaticMeshSlotRenameError::Index:
			return std::format("Static mesh material slot index {} is out of range for {} slots.", Error.Index, Error.SlotCount);
		case EStaticMeshSlotRenameError::EmptyName: return "Static mesh material slot name cannot be None.";
		case EStaticMeshSlotRenameError::DuplicateName:
			return std::format("Static mesh material slot name '{}' is already used by slot {}.", Error.Name, Error.ConflictingIndex);
		}
		return "Unknown static mesh slot rename failure.";
	}

	auto DStaticMesh::RenameMaterialSlot(uint32 SlotIndex, FName Name) -> std::expected<void, FStaticMeshSlotRenameError>
	{
		if (SlotIndex >= MaterialSlots.size())
		{
			return std::unexpected(FStaticMeshSlotRenameError{.Code = EStaticMeshSlotRenameError::Index, .Owner = FObjectKey(this),
				.Index = SlotIndex, .SlotCount = MaterialSlots.size(), .Name = Name.ToString()});
		}
		if (Name.IsNone())
		{
			return std::unexpected(FStaticMeshSlotRenameError{.Code = EStaticMeshSlotRenameError::EmptyName, .Owner = FObjectKey(this),
				.Index = SlotIndex, .SlotCount = MaterialSlots.size(), .Name = Name.ToString()});
		}
		const auto Existing = std::ranges::find(MaterialSlots, Name, &FMeshMaterialSlotDefinition::Name);
		if (Existing != MaterialSlots.end() && Existing != MaterialSlots.begin() + SlotIndex)
		{
			return std::unexpected(FStaticMeshSlotRenameError{.Code = EStaticMeshSlotRenameError::DuplicateName, .Owner = FObjectKey(this),
				.Index = SlotIndex, .SlotCount = MaterialSlots.size(),
				.ConflictingIndex = static_cast<uint64>(std::distance(MaterialSlots.begin(), Existing)), .Name = Name.ToString()});
		}
		if (MaterialSlots[SlotIndex].Name == Name)
		{
			return {};
		}
		MaterialSlots[SlotIndex].Name = Name;
		NotifyStaticMeshCompilationMutation(*this);
		if (RenderData && SlotIndex < RenderData->MaterialSlots.size())
			RenderData->MaterialSlots[SlotIndex].Name = Name.ToString();
		MarkPackageDirty();
		return {};
	}

	auto DStaticMesh::CommitPreparedMeshData(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition>* InMaterialSlots,
		FStaticMeshSource* PreparedSource,
		DAssetImportData* PreparedImportData, bool bAcceptGeometry) -> std::expected<void, FStaticMeshBuildFailure>
	{
		CheckStaticMeshUpdateThread();
		if (InRenderData == nullptr)
		{
			return std::unexpected(FStaticMeshBuildFailure{"Static-mesh publication requires render data.", EStaticMeshBuildStage::Application});
		}

#if DURIN_BUILD_DEBUG
		const FName DebugOwner = GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName()));
		InRenderData->SetResourceDebugOwner(DebugOwner);
#endif
		if (RenderData)
		{
			if (const auto Initialized = InitializeStaticMeshCandidate(*InRenderData); !Initialized)
				return Initialized;
		}

		const EStaticMeshRenderResourceState CandidateState =
			RenderData && GDynamicRHI != nullptr
				? EStaticMeshRenderResourceState::Ready
				: EStaticMeshRenderResourceState::Uninitialized;
		{
			std::optional<FStaticMeshRenderStateRecreateContext> RecreateContext;
			RecreateContext.emplace(this);
			const bool bGeometryChanged = bAcceptGeometry && (!PreparedSource
				|| Source.GetIdentity() != PreparedSource->GetIdentity()
				|| AcceptedGeometryNormalizedSize != NormalizedSize);
			if (bGeometryChanged) InvalidateCollisionData();
			if (bAcceptGeometry) AcceptedGeometryNormalizedSize = NormalizedSize;
			std::unique_ptr<FStaticMeshRenderData> OldRenderData =
				std::move(RenderData);
			if (InMaterialSlots != nullptr)
			{
				MaterialSlots = std::move(*InMaterialSlots);
			}
			if (PreparedSource) Source = std::move(*PreparedSource);
			RenderData = std::move(InRenderData);
			if (bAcceptGeometry) RefreshQualifiedBoxBodySetup();
			if (PreparedImportData) AssetImportData = PreparedImportData;
			PublishRenderResourceState(CandidateState);
			if (OldRenderData) RetireStaticMeshRenderData(OldRenderData);
		}
		RenderDataUpdateError = {};
		return {};
	}

	auto PublishStaticMeshRenderData(DStaticMesh& Mesh, std::unique_ptr<FStaticMeshRenderData> Render)
		-> std::expected<void, FStaticMeshBuildFailure>
	{
		return Mesh.CommitPreparedMeshData(std::move(Render), nullptr, nullptr, nullptr, false);
	}

	auto CommitStaticMeshBuild(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshRenderData> Render, FStaticMeshSource Source,
		const FStaticMeshReconciliationSnapshot& Snapshot,
		bool bMarkPackageDirty, const FAssetBuildTaskContext& Control,
		DAssetImportData* PreparedImportData,
		std::vector<FMeshMaterialSlotDefinition>* PreparedMaterialSlots,
		bool bPersistCollisionDerivedData) -> std::expected<void, FStaticMeshBuildFailure>
	{
		CheckStaticMeshUpdateThread();
		const auto Fail = [](FStaticMeshBuildFailure Error) -> std::expected<void, FStaticMeshBuildFailure> {
			return std::unexpected(std::move(Error));
		};
		if (Control.IsCancelled()) return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Application));
		if (!IsValid(&Mesh) || !Render || !Source.IsValid())
			return Fail(FStaticMeshBuildFailure{"StaticMesh publication requires a live owner, valid source and prepared render data.", EStaticMeshBuildStage::Application});
		if (PreparedImportData)
		{
			if (PreparedImportData->GetOuter() != &Mesh)
				return Fail(FStaticMeshBuildFailure{std::format(
					"StaticMesh provenance '{}' must be an owned inner of '{}'.",
					PreparedImportData->GetObjectPath(), Mesh.GetObjectPath()), EStaticMeshBuildStage::Application});
			if (const auto Validation = PreparedImportData->Validate(); !Validation)
				return Fail(FStaticMeshBuildFailure{FormatAssetImportDataError(Validation.error()), EStaticMeshBuildStage::Application});
		}
		const auto Current = FStaticMeshBuilder::Capture(Mesh);
		if (Current.SourceIdentity != Snapshot.SourceIdentity || Current.NormalizedSize != Snapshot.NormalizedSize
			|| Current.MaterialSlots.size() != Snapshot.MaterialSlots.size())
			return Fail(FStaticMeshBuildFailure{std::format(
				"StaticMesh owner '{}' changed during render construction (source changed {}, size {} -> {}, slots {} -> {}).",
				Mesh.GetObjectPath(), Current.SourceIdentity != Snapshot.SourceIdentity,
				Snapshot.NormalizedSize, Current.NormalizedSize,
				Snapshot.MaterialSlots.size(), Current.MaterialSlots.size()), EStaticMeshBuildStage::Application});
		for (size_t Index = 0; Index < Snapshot.MaterialSlots.size(); ++Index)
		{
			const auto& Expected = Snapshot.MaterialSlots[Index];
			const auto& Actual = Current.MaterialSlots[Index];
			if (Expected.Name != Actual.Name || Expected.SourceName != Actual.SourceName
				|| Expected.SourceMaterialIndex != Actual.SourceMaterialIndex || Expected.DefaultMaterial != Actual.DefaultMaterial)
				return Fail(FStaticMeshBuildFailure{std::format(
					"StaticMesh material bindings changed at slot {} (expected '{}', current '{}').",
					Index, Expected.Name.ToString(), Actual.Name.ToString()), EStaticMeshBuildStage::Application});
		}
		const auto& Slots = PreparedMaterialSlots ? *PreparedMaterialSlots : Snapshot.MaterialSlots;
		if (Render->MaterialSlots.size() != Slots.size())
			return Fail(FStaticMeshBuildFailure{"Prepared render material slots do not match publication input.", EStaticMeshBuildStage::Application});
		for (size_t Index = 0; Index < Slots.size(); ++Index)
			if (Render->MaterialSlots[Index].Name != Slots[Index].Name.ToString()
				|| Render->MaterialSlots[Index].SourceMaterialIndex != Slots[Index].SourceMaterialIndex)
				return Fail(FStaticMeshBuildFailure{"Prepared render material mapping changed before publication.", EStaticMeshBuildStage::Application});
		if (Control.IsCancelled()) return Fail(FStaticMeshBuildFailure::Cancelled(EStaticMeshBuildStage::Application));
		Source.ReleaseGeometry();
		if (const auto Published = Mesh.CommitPreparedMeshData(std::move(Render),
			PreparedMaterialSlots, &Source, PreparedImportData); !Published)
			return Fail(Published.error());
		if (Mesh.GetCollisionBuildStatus() == EPhysicsMeshBuildStatus::Unavailable)
			Mesh.ScheduleCollisionData(true, bPersistCollisionDerivedData);
		if (bMarkPackageDirty) Mesh.MarkPackageDirty();
		return {};
	}

	auto DStaticMesh::BeginDestroy() -> void
	{
		CancelStaticMeshCompilation(*this);
		if (FCookedMeshLoadManager* Manager = GetCookedMeshLoadManager())
			Manager->Cancel(FObjectKey(this));
		const EStaticMeshRenderResourceState State =
			LoadRenderResourceState();
		const bool bHasQueuedResourceWork =
			State != EStaticMeshRenderResourceState::Uninitialized
			&& State != EStaticMeshRenderResourceState::Released;
		ReleaseResources();
		if (bHasQueuedResourceWork)
		{
			ReleaseResourcesFence.BeginFence();
		}
		Super::BeginDestroy();
	}

	auto DStaticMesh::IsReadyForFinishDestroy() -> bool
	{
		return ReleaseResourcesFence.IsFenceComplete()
			&& Super::IsReadyForFinishDestroy();
	}

	auto DStaticMesh::FinishDestroy() -> void
	{
		check(ReleaseResourcesFence.IsFenceComplete());
		check(LoadRenderResourceState()
			== EStaticMeshRenderResourceState::Released);
		check(RenderData == nullptr
			|| RenderData->GetNumInitializedResources() == 0);
		AdvanceRenderResourceRevision();
		RenderData.reset();
		Super::FinishDestroy();
	}

	auto DStaticMesh::CreateDebugTriangle(DObject* Outer) -> DStaticMesh*
	{
		DStaticMesh* Mesh = NewObject<DStaticMesh>(Outer, "DebugStaticMesh");
		Mesh->MaterialSlots.push_back({.Name = FName("Default"), .SourceMaterialIndex = 0});
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->MaterialSlots.push_back({"Default", 0});
		FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
		LOD.ScreenSize = GenerateDefaultStaticMeshLODScreenSizes(1).front();
		LOD.VertexBuffers.PositionVertexBuffer.Init({
			FVector3f(-0.65f, -0.45f, 0.0f),
			FVector3f(0.65f, -0.45f, 0.0f),
			FVector3f(0.0f, 0.65f, 0.0f)
		});
		LOD.IndexBuffer.Init({0, 1, 2});
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<FVector3f>(
				3, FVector3f(0.0f, 0.0f, 1.0f)),
			std::vector<FVector4f>(
				3, FVector4f(1.0f, 0.0f, 0.0f, 1.0f)));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			{}, 3, 0);
		LOD.VertexBuffers.ColorVertexBuffer.Init({}, 3);
		LOD.VertexBuffers.Finalize(
			LOD.NumTexCoords, LOD.bHasColorVertexData);
		LOD.Sections.push_back({"Default", 0, 3, 0, 2, 0, {}});
		if (!FStaticMeshBuilder::FinalizeRenderData(*RenderData)) { MarkAsGarbage(Mesh); return nullptr; }
		if (const auto Published = Mesh->CommitPreparedMeshData(
			std::move(RenderData), nullptr); !Published)
		{
			DURIN_ERROR(
				"Failed to create debug static mesh: {}",
				(Published.error()).ToString());
			MarkAsGarbage(Mesh);
			return nullptr;
		}
		return Mesh;
	}

	auto DStaticMesh::InvalidateRenderData() -> void
	{
		CancelStaticMeshCompilation(*this);
		if (auto* Manager = GetCookedMeshLoadManager()) Manager->Cancel(FObjectKey(this));
		CookedLoadGeneration.fetch_add(1, std::memory_order_acq_rel);
		// Superseded cooked bytes must not restore geometry after a failed authored replacement.
		CookedRenderData = {};
		CookedCollisionData = {};
		CookedLoadPhase.store(ECookedMeshCpuPhase::Failed, std::memory_order_release);
		RetireStaticMeshRenderData(RenderData);
		RenderData.reset();
		PublishRenderResourceState(EStaticMeshRenderResourceState::Uninitialized);
		InvalidateCollisionData();
		// The qualified Box shape is derived from render bounds, unlike authored primitives.
		if (BodySetup && BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::None
			&& GetObjectPath().starts_with("/Engine/Models/Box"))
		{
			BodySetup = nullptr;
			RefreshCollisionBodies();
		}
	}

	auto FormatStaticMeshReplacementError(const FStaticMeshReplacementError& Error) -> std::string
	{
		switch (Error.Code)
		{
		case EStaticMeshReplacementError::None: return {};
		case EStaticMeshReplacementError::Source: return "StaticMesh replacement requires valid imported values and normalization.";
		case EStaticMeshReplacementError::MaterialSlots: return "StaticMesh replacement requires valid imported, render, and material values.";
		case EStaticMeshReplacementError::SlotName: return "StaticMesh replacement requires unique non-None material slots.";
		case EStaticMeshReplacementError::UVChannels: return "StaticMesh replacement exceeds the texture coordinate limit.";
		case EStaticMeshReplacementError::Payload: return Error.PayloadCause ? FormatStaticMeshPayloadError(*Error.PayloadCause) : "StaticMesh replacement payload is invalid.";
		case EStaticMeshReplacementError::Publication: return Error.PublicationCause ? (*Error.PublicationCause).ToString() : "StaticMesh replacement publication failed.";
		}
		return {};
	}

	auto DStaticMesh::ReplaceSourceRenderDataDestructively(
		FStaticMeshSource InSource,
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
		float InNormalizedSize) -> std::expected<void, FStaticMeshReplacementError>
	{
		CheckStaticMeshUpdateThread();
		if (!InSource.IsValid() || !std::isfinite(InNormalizedSize) || InNormalizedSize <= 0.0f)
		{
			FStaticMeshRenderStateRecreateContext RecreateContext(this);
			InvalidateRenderData();
			RenderDataUpdateError = {.Code = EStaticMeshReplacementError::Source,
				.SourceValid = InSource.IsValid(), .NormalizedSize = InNormalizedSize};
			DURIN_ERROR("Static mesh '{}' source replacement failed: {}", GetObjectPath(), FormatStaticMeshReplacementError(RenderDataUpdateError));
			return std::unexpected(RenderDataUpdateError);
		}
		NormalizedSize = InNormalizedSize;
		InSource.ReleaseGeometry();
		Source = std::move(InSource);
		return ReplaceRenderDataDestructively(std::move(InRenderData), std::move(InMaterialSlots));
	}

	auto DStaticMesh::ReplaceRenderDataDestructively(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots) -> std::expected<void, FStaticMeshReplacementError>
	{
		CheckStaticMeshUpdateThread();
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		const bool bInitializeResources = RenderData != nullptr;
		InvalidateRenderData();
		RenderDataUpdateError = {};
		if (const auto Replaced = ValidateAndReplaceRenderData(std::move(InRenderData), std::move(InMaterialSlots)); !Replaced)
		{
			RenderDataUpdateError = Replaced.error();
			DURIN_ERROR("Static mesh '{}' render-data replacement failed: {}", GetObjectPath(), FormatStaticMeshReplacementError(RenderDataUpdateError));
			return std::unexpected(RenderDataUpdateError);
		}
		CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
		ScheduleCollisionData(false);
		if (bInitializeResources) InitResources();
		return {};
	}

	auto DStaticMesh::ValidateAndReplaceRenderData(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots) -> std::expected<void, FStaticMeshReplacementError>
	{
		CheckStaticMeshUpdateThread();
		if (!InRenderData || InMaterialSlots.empty()
			|| InMaterialSlots.size() > MaximumMeshMaterialSlots
			|| InRenderData->MaterialSlots.size() != InMaterialSlots.size())
		{
			return std::unexpected(FStaticMeshReplacementError{.Code = EStaticMeshReplacementError::MaterialSlots, .RenderDataPresent = InRenderData != nullptr,
				.Actual = InMaterialSlots.size(), .Expected = InRenderData ? InRenderData->MaterialSlots.size() : 0});
		}
		std::unordered_set<FName> SlotNames;
		for (size_t Index = 0; Index < InMaterialSlots.size(); ++Index)
		{
			const auto& Slot = InMaterialSlots[Index];
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
			{
				return std::unexpected(FStaticMeshReplacementError{.Code = EStaticMeshReplacementError::SlotName, .Index = Index, .SlotName = Slot.Name.ToString()});
			}
		}
		for (size_t Index = 0; Index < InRenderData->LODResources.size(); ++Index)
		{
			const auto& LOD = InRenderData->LODResources[Index];
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return std::unexpected(FStaticMeshReplacementError{.Code = EStaticMeshReplacementError::UVChannels,
					.Actual = LOD.NumTexCoords, .Expected = MaxStaticMeshUVChannels, .Index = Index});
		}
		FStaticMeshPayloadData ValidatedPayload;
		if (const auto Result = MakeStaticMeshPayloadData(*InRenderData, ValidatedPayload); !Result)
		{
			return std::unexpected(FStaticMeshReplacementError{.Code = EStaticMeshReplacementError::Payload, .PayloadCause = std::make_shared<FStaticMeshPayloadError>(Result.error())});
		}
		if (const auto Finalized = FStaticMeshBuilder::FinalizeRenderData(*InRenderData); !Finalized)
			return std::unexpected(FStaticMeshReplacementError{.Code = EStaticMeshReplacementError::Publication, .PublicationCause = std::make_shared<FStaticMeshBuildFailure>(Finalized.error())});
		if (const auto Published = CommitPreparedMeshData(std::move(InRenderData), &InMaterialSlots); !Published)
		{
			return std::unexpected(FStaticMeshReplacementError{.Code = EStaticMeshReplacementError::Publication, .PublicationCause = std::make_shared<FStaticMeshBuildFailure>(Published.error())});
		}
		NotifyStaticMeshCompilationMutation(*this);
		return {};
	}

	auto DStaticMesh::SetMaterialSlotDefaultMaterial(
		uint32 SlotIndex, DMaterialInterface* Material) -> void
	{
		CheckStaticMeshUpdateThread();
		require(SlotIndex < MaterialSlots.size());
		FMeshMaterialSlotDefinition& Slot = MaterialSlots[SlotIndex];
		if (Slot.DefaultMaterial == Material) return;
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		Slot.DefaultMaterial = Material;
		NotifyStaticMeshCompilationMutation(*this);
		MarkPackageDirty();
	}
}
