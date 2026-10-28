#include "StaticMesh/StaticMesh.h"
#include "StaticMeshBuildPrivate.h"

#include "Asset/CookedMeshLoadManager.h"

#include "DObject/Package.h"

#include "Asset/Asset.h"
#include "Asset/AssetCompilingManager.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "DObject/PropertyChange.h"
#include "Hash/XxHash.h"
#include "Logging/LogMacros.h"
#include "Math/Operations.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsMeshCompilation.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshBuild.h"
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
			FStaticMeshRenderData& Candidate) -> std::expected<void, std::string>
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
				return std::unexpected(std::string{"Static-mesh candidate resource initialization failed."});
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
		DAssetImportData* PreparedImportData, bool bAcceptGeometry) -> std::expected<void, std::string>
	{
		CheckStaticMeshUpdateThread();
		if (InRenderData == nullptr)
		{
			return std::unexpected(std::string{"Static-mesh publication requires render data."});
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
#if DURIN_WITH_EDITORONLY_DATA
			const bool bGeometryChanged = bAcceptGeometry && (!PreparedSource
				|| Source.GetIdentity() != PreparedSource->GetIdentity()
				|| AcceptedGeometryNormalizedSize != NormalizedSize);
#else
			if (PreparedSource || PreparedImportData)
				return std::unexpected(std::string{
					"Authored StaticMesh publication requires editor-only data support."});
			const bool bGeometryChanged = bAcceptGeometry
				&& AcceptedGeometryNormalizedSize != NormalizedSize;
#endif
			if (bGeometryChanged) InvalidateCollisionData();
			if (bAcceptGeometry) AcceptedGeometryNormalizedSize = NormalizedSize;
			std::unique_ptr<FStaticMeshRenderData> OldRenderData =
				std::move(RenderData);
			if (InMaterialSlots != nullptr)
			{
				MaterialSlots = std::move(*InMaterialSlots);
			}
#if DURIN_WITH_EDITORONLY_DATA
			if (PreparedSource) Source = std::move(*PreparedSource);
#endif
			RenderData = std::move(InRenderData);
			if (bAcceptGeometry) RefreshQualifiedBoxBodySetup();
#if DURIN_WITH_EDITORONLY_DATA
			if (PreparedImportData) AssetImportData = PreparedImportData;
#endif
			PublishRenderResourceState(CandidateState);
			if (OldRenderData) RetireStaticMeshRenderData(OldRenderData);
			if (bGeometryChanged) NotifyCollisionSourceChanged();
		}

		return {};
	}

	auto PublishStaticMeshRenderData(DStaticMesh& Mesh, std::unique_ptr<FStaticMeshRenderData> Render)
		-> std::expected<void, std::string>
	{
		return Mesh.CommitPreparedMeshData(std::move(Render), nullptr, nullptr, nullptr, false);
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto CommitStaticMeshBuild(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshRenderData> Render, FStaticMeshSource Source,
		const FStaticMeshReconciliationSnapshot& Snapshot,
		bool bMarkPackageDirty,
		DAssetImportData* PreparedImportData,
		std::vector<FMeshMaterialSlotDefinition>* PreparedMaterialSlots,
		bool bPersistCollisionDerivedData) -> std::expected<void, std::string>
	{
		CheckStaticMeshUpdateThread();
		const auto Fail = [](std::string Error) -> std::expected<void, std::string> {
			return std::unexpected(std::move(Error));
		};
		if (!IsValid(&Mesh) || !Render || !Source.IsValid())
			return Fail(std::string{"StaticMesh publication requires a live owner, valid source and prepared render data."});
		if (PreparedImportData)
		{
			if (PreparedImportData->GetOuter() != &Mesh)
				return Fail(std::string{std::format(
					"StaticMesh provenance '{}' must be an owned inner of '{}'.",
					PreparedImportData->GetObjectPath(), Mesh.GetObjectPath())});
			if (const auto Validation = PreparedImportData->Validate(); !Validation)
				return Fail(std::string{FormatAssetImportDataError(Validation.error())});
		}
		const auto Current = CaptureStaticMeshReconciliation(Mesh);
		if (Current.SourceIdentity != Snapshot.SourceIdentity || Current.NormalizedSize != Snapshot.NormalizedSize
			|| Current.MaterialSlots.size() != Snapshot.MaterialSlots.size())
			return Fail(std::string{std::format(
				"StaticMesh owner '{}' changed during render construction (source changed {}, size {} -> {}, slots {} -> {}).",
				Mesh.GetObjectPath(), Current.SourceIdentity != Snapshot.SourceIdentity,
				Snapshot.NormalizedSize, Current.NormalizedSize,
				Snapshot.MaterialSlots.size(), Current.MaterialSlots.size())});
		for (size_t Index = 0; Index < Snapshot.MaterialSlots.size(); ++Index)
		{
			const auto& Expected = Snapshot.MaterialSlots[Index];
			const auto& Actual = Current.MaterialSlots[Index];
			if (Expected.Name != Actual.Name || Expected.SourceName != Actual.SourceName
				|| Expected.SourceMaterialIndex != Actual.SourceMaterialIndex || Expected.DefaultMaterial != Actual.DefaultMaterial)
				return Fail(std::string{std::format(
					"StaticMesh material bindings changed at slot {} (expected '{}', current '{}').",
					Index, Expected.Name.ToString(), Actual.Name.ToString())});
		}
		const auto& Slots = PreparedMaterialSlots ? *PreparedMaterialSlots : Snapshot.MaterialSlots;
		if (Render->MaterialSlots.size() != Slots.size())
			return Fail(std::string{"Prepared render material slots do not match publication input."});
		for (size_t Index = 0; Index < Slots.size(); ++Index)
			if (Render->MaterialSlots[Index].Name != Slots[Index].Name.ToString()
				|| Render->MaterialSlots[Index].SourceMaterialIndex != Slots[Index].SourceMaterialIndex)
				return Fail(std::string{"Prepared render material mapping changed before publication."});
		Source.ReleaseGeometry();
		if (const auto Published = Mesh.CommitPreparedMeshData(std::move(Render),
			PreparedMaterialSlots, &Source, PreparedImportData); !Published)
			return Fail(Published.error());
		if (Mesh.GetCollisionBuildStatus() == EPhysicsMeshBuildStatus::Unavailable)
			Mesh.ScheduleCollisionData(true, bPersistCollisionDerivedData);
		if (bMarkPackageDirty) Mesh.MarkPackageDirty();
		return {};

	}

#endif

	auto DStaticMesh::BlockOnAnyAsyncBuild() -> void
	{
		CheckStaticMeshUpdateThread();
		if (bCompilationMutationBlocked) return;
		bCompilationMutationBlocked = true;
		auto* Body = GetBodySetup();
		const bool bBodyWasBlocked = Body && Body->bCompilationMutationBlocked;
		if (Body) Body->bCompilationMutationBlocked = true;
		struct FMutationGuard
		{
			FObjectKey Owner;
			FObjectKey Body;
			bool bBodyWasBlocked;
			~FMutationGuard()
			{
				if (auto* Mesh = Cast<DStaticMesh>(ResolveObjectKey(Owner))) Mesh->bCompilationMutationBlocked = false;
				if (auto* Setup = Cast<DBodySetup>(ResolveObjectKey(Body))) Setup->bCompilationMutationBlocked = bBodyWasBlocked;
			}
		} Guard{FObjectKey(this), FObjectKey(Body), bBodyWasBlocked};
		CancelCompilation();
		AssetPrivate::CancelPhysicsMeshCompilationForOwner(*this);
		if (auto* Body = GetBodySetup()) Body->AbortPhysicsMeshAsyncCreation();
		FinishCompilation();
		AssetPrivate::FinishPhysicsMeshCompilationForOwner(*this);
		if (auto* Body = GetBodySetup()) Body->FinishPhysicsMeshes();
	}
	auto AssetPrivate::IsStaticMeshCompilationMutationBlocked(const DStaticMesh& Mesh) -> bool
	{
		return Mesh.IsCompilationMutationBlocked();
	}
	auto DStaticMesh::BeginDestroy() -> void
	{
		this->CancelCompilation();
		AssetPrivate::CancelPhysicsMeshCompilationForOwner(*this);
		if (auto* Body = GetBodySetup()) Body->AbortPhysicsMeshAsyncCreation();
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
		Mesh->MaterialSlots.push_back({.Name = FName("Default")});
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
		RenderData->RecalculateBounds();
		if (const auto Published = Mesh->CommitPreparedMeshData(
			std::move(RenderData), nullptr); !Published)
		{
			DURIN_ERROR(
				"Failed to create debug static mesh: {}",
				Published.error());
			MarkAsGarbage(Mesh);
			return nullptr;
		}
		return Mesh;
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DStaticMesh::SetImportedSource(FStaticMeshSource InSource,
		std::vector<FMeshMaterialSlotDefinition> Slots, DAssetImportData& ImportData)
		-> std::expected<void, std::string>
	{
		CheckStaticMeshUpdateThread();
		if (AssetPrivate::IsStaticMeshCompilationMutationBlocked(*this))
			return std::unexpected(std::string("StaticMesh mutation is retiring prior compilation."));
		if (!InSource.IsValid() || !std::isfinite(NormalizedSize) || NormalizedSize <= 0)
			return std::unexpected(std::string("StaticMesh import requires valid source and normalization."));
		if (ImportData.GetOuter() != this)
			return std::unexpected(std::string("StaticMesh import metadata must be owned by the mesh."));
		if (const auto Valid = ImportData.Validate(); !Valid)
			return std::unexpected(FormatAssetImportDataError(Valid.error()));
		if (Slots.size() > MaximumMeshMaterialSlots)
			return std::unexpected(std::string("StaticMesh import exceeds the material-slot limit."));
		std::unordered_set<FName> Names;
		std::unordered_set<uint32> SourceIndices;
		for (const auto& Slot : Slots)
			if (Slot.Name.IsNone() || Slot.SourceName.size() > 4096 || !Names.insert(Slot.Name).second
				|| !SourceIndices.insert(Slot.SourceMaterialIndex).second)
				return std::unexpected(std::string("StaticMesh import requires bounded unique material slots and source indices."));
		{
			const auto Geometry = InSource.AcquireGeometry();
			if (!Geometry) return std::unexpected(FormatStaticMeshSourceError(Geometry.error()));
			for (const auto& Group : (*Geometry)->PolygonGroups)
				if (!SourceIndices.contains(Group.SourceMaterialIndex))
					return std::unexpected(std::string("StaticMesh import requires a material slot for every source polygon group."));
		}
		BlockOnAnyAsyncBuild();
		// Consumers must never observe new slots with old render or collision geometry.
		{
			FStaticMeshRenderStateRecreateContext Refresh(this);
			InvalidateRenderData();
			InSource.ReleaseGeometry();
			Source = std::move(InSource);
			MaterialSlots = std::move(Slots);
			AssetImportData = &ImportData;
			AcceptedGeometryNormalizedSize = NormalizedSize;
			NotifyCollisionSourceChanged();
		}
		MarkPackageDirty();
		return {};
	}
#endif

	auto DStaticMesh::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
#if DURIN_WITH_EDITORONLY_DATA
		if (Event.MemberProperty && (Event.MemberProperty->NamePrivate == FName("ComplexCollisionMesh")
			|| Event.MemberProperty->NamePrivate == FName("LODForCollision")))
		{
			if (ComplexCollisionMesh == this) ComplexCollisionMesh = nullptr;
			NotifyCollisionSourceChanged(true);
			return;
		}
		if (Event.MemberProperty || Event.Phase != EPropertyChangePhase::Committed || IsTemplateObject()
			|| GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		if (const auto Admitted = Build(EStaticMeshBuildMode::Asynchronous, {.bMarkPackageDirty = false}); !Admitted)
			DURIN_ERROR("StaticMesh build admission failed for {}: {}", GetObjectPath(), Admitted.error());
#endif
	}

	auto DStaticMesh::InvalidateRenderData() -> void
	{
		this->CancelCompilation();
		AssetPrivate::CancelPhysicsMeshCompilationForOwner(*this);
		if (auto* Body = GetBodySetup()) Body->AbortPhysicsMeshAsyncCreation();
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
		NotifyCollisionSourceChanged();
		// The qualified Box shape is derived from render bounds, unlike authored primitives.
		if (BodySetup && BodySetup->GetCollisionSourceMode() == EBodySetupCollisionSourceMode::None
			&& GetObjectPath().starts_with("/Engine/Models/Box"))
		{
			BodySetup = nullptr;
			RefreshCollisionBodies();
		}
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DStaticMesh::ReplaceSourceRenderDataDestructively(
		FStaticMeshSource InSource,
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
		float InNormalizedSize) -> std::expected<void, std::string>
	{
		CheckStaticMeshUpdateThread();
		if (!InSource.IsValid() || !std::isfinite(InNormalizedSize) || InNormalizedSize <= 0.0f)
		{
			FStaticMeshRenderStateRecreateContext RecreateContext(this);
			InvalidateRenderData();
			return std::unexpected(std::string("StaticMesh replacement requires valid source and normalization."));
		}
		NormalizedSize = InNormalizedSize;
		InSource.ReleaseGeometry();
		Source = std::move(InSource);
		return ReplaceRenderDataDestructively(std::move(InRenderData), std::move(InMaterialSlots));
	}
#endif

	auto DStaticMesh::ReplaceRenderDataDestructively(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots) -> std::expected<void, std::string>
	{
		CheckStaticMeshUpdateThread();
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		const bool bInitializeResources = RenderData != nullptr;
		InvalidateRenderData();

		if (const auto Replaced = ValidateAndReplaceRenderData(std::move(InRenderData), std::move(InMaterialSlots)); !Replaced)
		{
			return Replaced;
		}
		CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
		ScheduleCollisionData(false);
		if (bInitializeResources) InitResources();
		return {};
	}

	auto DStaticMesh::ValidateAndReplaceRenderData(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots) -> std::expected<void, std::string>
	{
		CheckStaticMeshUpdateThread();
		if (!InRenderData || InMaterialSlots.empty()
			|| InMaterialSlots.size() > MaximumMeshMaterialSlots
			|| InRenderData->MaterialSlots.size() != InMaterialSlots.size())
		{
			return std::unexpected(std::string("StaticMesh replacement requires matching nonempty material slots and render data."));
		}
		std::unordered_set<FName> SlotNames;
		for (size_t Index = 0; Index < InMaterialSlots.size(); ++Index)
		{
			const auto& Slot = InMaterialSlots[Index];
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
			{
				return std::unexpected(std::string("StaticMesh replacement requires unique non-None material slots."));
			}
		}
		for (size_t Index = 0; Index < InRenderData->LODResources.size(); ++Index)
		{
			const auto& LOD = InRenderData->LODResources[Index];
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
				return std::unexpected(std::string("StaticMesh replacement exceeds the texture coordinate limit."));
		}
		if (const auto Result = ValidateStaticMeshRenderData(*InRenderData); !Result)
		{
			return std::unexpected(FormatStaticMeshPayloadError(Result.error()));
		}
		for (size_t Index = 0; Index < InMaterialSlots.size(); ++Index)
		{
			InRenderData->MaterialSlots[Index].Name = InMaterialSlots[Index].Name.ToString();
#if DURIN_WITH_EDITORONLY_DATA
			InRenderData->MaterialSlots[Index].SourceMaterialIndex = InMaterialSlots[Index].SourceMaterialIndex;
#endif
		}
		InRenderData->RecalculateBounds();
		if (const auto Published = CommitPreparedMeshData(std::move(InRenderData), &InMaterialSlots); !Published)
		{
			return std::unexpected(Published.error());
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
	auto DStaticMesh::PrepareRenderResources(uint32& RemainingRequests) -> std::expected<bool, std::string>
	{
		if (IsCompiling()) return false;
		const auto Diagnostic = GetCompilationDiagnostic();
		if (Diagnostic.Status && *Diagnostic.Status != EStaticMeshCompilationStatus::Succeeded)
			return std::unexpected(FormatStaticMeshCompilationDiagnostic(Diagnostic));
		const auto Status = GetRenderDataLoadStatus();
		if (Status.CpuPhase == ECookedMeshCpuPhase::Failed || Status.CpuPhase == ECookedMeshCpuPhase::Cancelled || Status.GpuPhase == ECookedMeshGpuPhase::Failed)
			return std::unexpected("required mesh render data failed.");
		if (Status.CpuPhase == ECookedMeshCpuPhase::CpuReady && Status.GpuPhase == ECookedMeshGpuPhase::Ready) return true;
		if (RemainingRequests != 0)
		{
			if (const auto* Manager = GetCookedMeshLoadManager())
			{
				const auto Work = Manager->GetDiagnostics();
				if (Work.InFlightCount + Work.PendingRequestCount >= 8) return false;
			}
			--RemainingRequests;
			(void)RequestRenderDataAndResources();
		}
		return false;
	}

}
