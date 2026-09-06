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
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Physics/BodySetup.h"
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
			FStaticMeshRenderData& Candidate,
			std::string& OutError) -> bool
		{
			if (GDynamicRHI == nullptr)
			{
				OutError.clear();
				return true;
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
				OutError =
					"Static-mesh candidate resource initialization failed.";
				return false;
			}
			OutError.clear();
			return true;
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




		auto IsCanonicalStaticMeshHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32 && std::ranges::all_of(Hash, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
		}


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

	auto FStaticMeshImportSettings::IsValid(std::string* OutError) const -> bool
	{
		FVector3f UnusedVector;
		uint32 ForwardComponent = 0;
		uint32 RightComponent = 0;
		uint32 UpComponent = 0;
		const bool bAxesKnown = ImportAxisVector(ForwardAxis, UnusedVector, ForwardComponent)
			&& ImportAxisVector(RightAxis, UnusedVector, RightComponent)
			&& ImportAxisVector(UpAxis, UnusedVector, UpComponent);
		if (!bAxesKnown)
		{
			if (OutError) *OutError = "The import coordinate system contains an unknown axis.";
			return false;
		}
		if (ForwardComponent == RightComponent || ForwardComponent == UpComponent || RightComponent == UpComponent)
		{
			if (OutError) *OutError = "Forward, Right, and Up must use X, Y, and Z exactly once.";
			return false;
		}
		if (OutError) OutError->clear();
		return true;
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
				Manager->Cancel(MakeObjectHandle(this));
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
				Manager->Finish(MakeObjectHandle(this));
			}
		}
		if (!RenderData && CookedLoadPhase.load(std::memory_order_acquire)
			!= ECookedMeshCpuPhase::Failed
			&& GetAssetRuntimeConfiguration().RequiresCookedPayload()
			&& CookedRenderData.GetMetadata().LogicalSize != 0)
		{
			CookedLoadPhase.store(ECookedMeshCpuPhase::Reading, std::memory_order_release);
			std::string Error;
			if (!LoadCookedRenderData(Error))
			{
				CookedLoadPhase.store(ECookedMeshCpuPhase::Failed, std::memory_order_release);
				return {.Status = GetRenderDataLoadStatus(), .Message = std::move(Error)};
			}
			CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
		}
		if (RenderData)
		{
			CookedLoadPhase.store(ECookedMeshCpuPhase::CpuReady, std::memory_order_release);
		}
		FCookedMeshBlockingResult Result{.Status = GetRenderDataLoadStatus()};
		if (!Result.Status.HasCpuData()) Result.Message = "StaticMesh CPU render data is unavailable.";
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

	auto DStaticMesh::RenameMaterialSlot(uint32 SlotIndex, FName Name, std::string& OutError) -> bool
	{
		if (SlotIndex >= MaterialSlots.size())
		{
			OutError = std::format("Static mesh material slot index {} is out of range.", SlotIndex);
			return false;
		}
		if (Name.IsNone())
		{
			OutError = "Static mesh material slot name cannot be None.";
			return false;
		}
		const auto Existing = std::ranges::find(MaterialSlots, Name, &FMeshMaterialSlotDefinition::Name);
		if (Existing != MaterialSlots.end() && Existing != MaterialSlots.begin() + SlotIndex)
		{
			OutError = std::format("Static mesh material slot name '{}' is already in use.", Name.ToString());
			return false;
		}
		if (MaterialSlots[SlotIndex].Name == Name)
		{
			OutError.clear();
			return true;
		}
		MaterialSlots[SlotIndex].Name = Name;
		NotifyStaticMeshCompilationMutation(*this);
		if (RenderData && SlotIndex < RenderData->MaterialSlots.size())
			RenderData->MaterialSlots[SlotIndex].Name = Name.ToString();
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::CommitRenderDataCandidate(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition>* InMaterialSlots,
		std::string& OutError,
		bool bBuildAuthoredCollision, FStaticMeshAuthoredCandidate* AuthoredCandidate,
		DAssetImportData* PreparedImportData) -> bool
	{
		CheckStaticMeshUpdateThread();
		if (InRenderData == nullptr)
		{
			OutError = "Static-mesh publication requires render data.";
			return false;
		}
		if (!AuthoredCandidate && !ValidateStaticMeshLODScreenSizes(
			InRenderData->LODResources, OutError))
		{
			return false;
		}
		if (!AuthoredCandidate)
		{
			InRenderData->RecalculateBounds();
#if DURIN_WITH_EDITOR
			for (FStaticMeshLODResources& LOD : InRenderData->LODResources)
				LOD.RayQueryAcceleration = BuildStaticMeshRayQueryAcceleration(LOD);
#endif
		}
		FCollisionGeometryRef CollisionSimple;
		FCollisionGeometryRef CollisionComplex;
		const bool bHasAuthoredCollision = bBuildAuthoredCollision && BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		if (AuthoredCandidate)
		{
			CollisionSimple = AuthoredCandidate->Collision.Simple;
			CollisionComplex = AuthoredCandidate->Collision.Complex;
		}
		if (!AuthoredCandidate && bHasAuthoredCollision && !BuildCollisionCandidate(
			*InRenderData,
			BodySetup->GetCollisionSourceMode(),
			BodySetup->GetCollisionQueryPolicy(),
			CollisionSimple,
			CollisionComplex,
			OutError)) return false;

#if DURIN_BUILD_DEBUG
		const FName DebugOwner = GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName()));
		InRenderData->SetResourceDebugOwner(DebugOwner);
#endif
		if (RenderData && !InitializeStaticMeshCandidate(*InRenderData, OutError))
		{
			return false;
		}

		const EStaticMeshRenderResourceState CandidateState =
			RenderData && GDynamicRHI != nullptr
				? EStaticMeshRenderResourceState::Ready
				: EStaticMeshRenderResourceState::Uninitialized;
		{
			std::optional<FStaticMeshRenderStateRecreateContext> RecreateContext;
			if (RenderData || AuthoredCandidate) RecreateContext.emplace(this);
			std::unique_ptr<FStaticMeshRenderData> OldRenderData =
				std::move(RenderData);
			if (InMaterialSlots != nullptr)
			{
				MaterialSlots = std::move(*InMaterialSlots);
			}
			if (AuthoredCandidate)
			{
				ImportedData = std::move(AuthoredCandidate->Request.Source);
				NormalizedSize = AuthoredCandidate->Request.NormalizedSize;
			}
			RenderData = std::move(InRenderData);
			RefreshQualifiedBoxBodySetup();
			if (bHasAuthoredCollision)
			{
				const bool bPublished = BodySetup->SetCollisionGeometry(
					CollisionSimple, CollisionComplex);
				check(bPublished);
			}
			if (PreparedImportData) AssetImportData = PreparedImportData;
			PublishRenderResourceState(CandidateState);
			if (OldRenderData) RetireStaticMeshRenderData(OldRenderData);
		}
		OutError.clear();
		return true;
	}

	auto ApplyStaticMeshAuthoredCandidate(DStaticMesh& Mesh,
		std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate,
		const FStaticMeshReconciliationSnapshot& Snapshot, std::string& OutError,
		bool bMarkPackageDirty, const FStaticMeshBuildExecutionControl& Control,
		DAssetImportData* PreparedImportData) -> FStaticMeshBuildOutcome
	{
		CheckStaticMeshUpdateThread();
		OutError.clear();
		const auto Fail = [&](std::string_view Message, EStaticMeshBuildStatus Status = EStaticMeshBuildStatus::Failed) {
			OutError = Message;
			return FStaticMeshBuildOutcome(Status, OutError);
		};
		if (Control.IsCancelled()) return Fail("StaticMesh application was cancelled.", EStaticMeshBuildStatus::Cancelled);
		if (!IsValid(&Mesh) || !Candidate || !Candidate->Render.RenderData)
			return Fail("StaticMesh application requires a live asset and a complete candidate.");
		if (PreparedImportData && (PreparedImportData->GetOuter() != &Mesh || !PreparedImportData->Validate(OutError)))
			return Fail(OutError.empty() ? "StaticMesh provenance must be a validated owned inner." : OutError);
		const auto Current = CaptureStaticMeshReconciliation(Mesh);
		if (Current.SourceIdentity != Snapshot.SourceIdentity || Current.NormalizedSize != Snapshot.NormalizedSize
			|| Current.Body != Snapshot.Body || Current.BodyRevision != Snapshot.BodyRevision
			|| Current.CollisionMode != Snapshot.CollisionMode || Current.CollisionPolicy != Snapshot.CollisionPolicy
			|| Current.MaterialSlots.size() != Snapshot.MaterialSlots.size())
			return Fail("StaticMesh owner changed during candidate construction.");
		const auto& Request = Candidate->Request;
		if (Request.NormalizedSize != Snapshot.NormalizedSize || Request.CollisionMode != Snapshot.CollisionMode
			|| Request.CollisionPolicy != Snapshot.CollisionPolicy || Request.MaterialSlots.size() != Snapshot.MaterialSlots.size())
			return Fail("StaticMesh candidate does not match its application snapshot.");
		for (size_t Index = 0; Index < Snapshot.MaterialSlots.size(); ++Index)
		{
			const auto& Expected = Snapshot.MaterialSlots[Index];
			const auto& Actual = Current.MaterialSlots[Index];
			const auto& Input = Request.MaterialSlots[Index];
			if (Expected.Name != Actual.Name || Expected.SourceName != Actual.SourceName
				|| Expected.SourceMaterialIndex != Actual.SourceMaterialIndex || Expected.DefaultMaterial != Actual.DefaultMaterial
				|| Expected.Name != Input.Name || Expected.SourceName != Input.SourceName
				|| Expected.SourceMaterialIndex != Input.SourceMaterialIndex)
				return Fail("StaticMesh material bindings changed during candidate construction.");
		}
		for (size_t Index = 0; Index < Candidate->Render.MaterialSlots.size() && Index < Snapshot.MaterialSlots.size(); ++Index)
			Candidate->Render.MaterialSlots[Index].DefaultMaterial = Snapshot.MaterialSlots[Index].DefaultMaterial;
		if (Control.IsCancelled()) return Fail("StaticMesh application was cancelled.", EStaticMeshBuildStatus::Cancelled);
		const bool bSlotMetadataChanged = Candidate->Render.bSlotMetadataChanged;
		if (!Mesh.CommitRenderDataCandidate(std::move(Candidate->Render.RenderData),
			&Candidate->Render.MaterialSlots, OutError, true, Candidate.get(), PreparedImportData))
			return {EStaticMeshBuildStatus::Failed, OutError};
		if (bSlotMetadataChanged)
			ReportAssetLoadMutation(&Mesh, "Engine.StaticMesh.MaterialSlotsV1",
				"Static mesh material-slot identity metadata was upgraded.", EAssetLoadMutationKind::Upgrade);
		if (bMarkPackageDirty || bSlotMetadataChanged) Mesh.MarkPackageDirty();
		return {EStaticMeshBuildStatus::Succeeded};
	}

	auto DStaticMesh::BeginDestroy() -> void
	{
		CancelStaticMeshCompilation(*this);
		if (FCookedMeshLoadManager* Manager = GetCookedMeshLoadManager())
			Manager->Cancel(MakeObjectHandle(this));
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
		std::string PublishError;
		if (!Mesh->CommitRenderDataCandidate(
			std::move(RenderData), nullptr, PublishError))
		{
			DURIN_ERROR(
				"Failed to create debug static mesh: {}",
				PublishError);
			MarkAsGarbage(Mesh);
			return nullptr;
		}
		return Mesh;
	}

	auto DStaticMesh::SetImportedRenderData(
		FStaticMeshImportedData InImportedData,
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
		float InNormalizedSize, std::string& OutError) -> bool
	{
		CheckStaticMeshUpdateThread();
		if (!InImportedData.IsValid()
			|| !std::isfinite(InNormalizedSize) || InNormalizedSize <= 0.0f)
		{
			OutError = "StaticMesh replacement requires valid imported values and normalization.";
			return false;
		}
		if (!SetRenderData(std::move(InRenderData), std::move(InMaterialSlots), OutError))
			return false;
		NormalizedSize = InNormalizedSize;
		// Assets retain canonical storage. Operation handles and other source copies remain valid.
		InImportedData.ReleaseGeometry();
		ImportedData = std::move(InImportedData);
		NotifyStaticMeshCompilationMutation(*this);
		return true;
	}

	auto DStaticMesh::SetRenderData(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
		std::string& OutError) -> bool
	{
		CheckStaticMeshUpdateThread();
		if (!InRenderData || InMaterialSlots.empty()
			|| InMaterialSlots.size() > MaximumMeshMaterialSlots
			|| InRenderData->MaterialSlots.size() != InMaterialSlots.size())
		{
			OutError = "StaticMesh replacement requires valid imported, render, and material values.";
			return false;
		}
		std::unordered_set<FName> SlotNames;
		for (const FMeshMaterialSlotDefinition& Slot : InMaterialSlots)
		{
			if (Slot.Name.IsNone() || !SlotNames.insert(Slot.Name).second)
			{
				OutError = "StaticMesh replacement requires unique non-None material slots.";
				return false;
			}
		}
		for (const FStaticMeshLODResources& LOD : InRenderData->LODResources)
			if (LOD.NumTexCoords > MaxStaticMeshUVChannels)
			{
				OutError = "StaticMesh replacement exceeds the texture coordinate limit.";
				return false;
			}
		FStaticMeshPayloadData ValidatedPayload;
		if (!MakeStaticMeshPayloadData(*InRenderData, ValidatedPayload, OutError)
			|| !CommitRenderDataCandidate(std::move(InRenderData), &InMaterialSlots, OutError))
			return false;
		NotifyStaticMeshCompilationMutation(*this);
		OutError.clear();
		return true;
	}

	auto DStaticMesh::PublishAssetImportData(
		DAssetImportData& Value, std::string& OutError) -> bool
	{
		if (Value.GetOuter() != this)
		{
			OutError = "StaticMesh import data must be an owned inner object.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		if (AssetImportData == &Value) { OutError.clear(); return true; }
		AssetImportData = &Value;
		NotifyStaticMeshCompilationMutation(*this);
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::SetImportedDefaultMaterial(
		uint32 SourceMaterialIndex,
		DMaterialInterface* Material,
		std::string& OutError) -> bool
	{
		const auto Slot = std::ranges::find(
			MaterialSlots,
			SourceMaterialIndex,
			&FMeshMaterialSlotDefinition::SourceMaterialIndex);
		if (Slot == MaterialSlots.end())
		{
			OutError = std::format(
				"Static mesh has no slot for source material {}.", SourceMaterialIndex);
			return false;
		}
		if (std::ranges::find(
			std::next(Slot),
			MaterialSlots.end(),
			SourceMaterialIndex,
			&FMeshMaterialSlotDefinition::SourceMaterialIndex) != MaterialSlots.end())
		{
			OutError = std::format(
				"Static mesh has ambiguous slots for source material {}.", SourceMaterialIndex);
			return false;
		}
		if (Slot->DefaultMaterial == Material) { OutError.clear(); return true; }
		FStaticMeshRenderStateRecreateContext RecreateContext(this);
		Slot->DefaultMaterial = Material;
		NotifyStaticMeshCompilationMutation(*this);
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

}
