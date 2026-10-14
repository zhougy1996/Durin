#pragma once

#include "CoreMinimal.h"

#include "Asset/CookedMeshLoading.h"

#include "Asset/AssetImportData.h"
#include "Asset/BulkData.h"
#include "Asset/Cook.h"
#include "StaticMesh/StaticMeshSource.h"
#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"
#include "Materials/MeshMaterialSlot.h"
#include "Physics/BodySetupTypes.h"
#include "Physics/CollisionDataProvider.h"
#include "RenderingThread.h"

#include "StaticMesh.gen.h"

namespace Durin
{
	struct FStaticMeshSource;
	class DAssetImportData;

	struct FStaticMeshCompilationRequest;
	struct FStaticMeshCompilationDiagnostic;
	struct FAssetCompileProcessResult;

	enum class EStaticMeshBuildMode : uint8 { Synchronous, Asynchronous };
	enum class EStaticMeshCompilationPriority : uint8 { Background, Interactive };
	struct FStaticMeshBuildOptions
	{
		EStaticMeshCompilationPriority Priority = EStaticMeshCompilationPriority::Background;
		bool bPersistDerivedData = true;
		bool bMarkPackageDirty = true;
	};

	class DBodySetup;
	class FStaticMeshCompilingManager;
	struct FStaticMeshReconciliationSnapshot;
	class FCollisionGeometryRef;
	enum class EBodySetupCollisionSourceMode : uint8;
	enum class EBodySetupCollisionQueryPolicy : uint8;
	enum class ECollisionGeometryKind : uint8;

	enum class EStaticMeshSlotRenameError : uint8 { None, Index, EmptyName, DuplicateName };
	struct FStaticMeshSlotRenameError
	{
		EStaticMeshSlotRenameError Code = EStaticMeshSlotRenameError::None;
		FObjectKey Owner;
		uint64 Index = 0;
		uint64 SlotCount = 0;
		uint64 ConflictingIndex = 0;
		std::string Name;
	};

	ENGINE_API auto FormatStaticMeshSlotRenameError(const FStaticMeshSlotRenameError& Error) -> std::string;

	// Reports only the semantic render-resource states required by nonblocking consumers.
	enum class EStaticMeshRenderResourceReadiness : uint8
	{
		Unavailable,
		Queued,
		Ready,
		Failed
	};

	// Pairs readiness with the asset-local revision that must still match before publication.
	// Revisions are never transferred between assets or reused: construction starts unavailable at
	// a non-zero revision, and every accepted CPU-data publication, initialization result, imported-
	// state exchange, resource invalidation, release, or destruction boundary advances it.
	struct FStaticMeshRenderResourceStatus
	{
		EStaticMeshRenderResourceReadiness Readiness =
			EStaticMeshRenderResourceReadiness::Unavailable;
		uint64 Revision = 0;

		auto IsReady() const -> bool
		{
			return Readiness == EStaticMeshRenderResourceReadiness::Ready
				&& Revision != 0;
		}
	};


	struct FStaticMeshRenderData;

	// Owner-thread readiness of derived collision for the current mesh configuration.


	// Owns imported mesh metadata, material slots, and rebuilt render resources.
	DCLASS()
	class DStaticMesh : public DObject, public IInterface_CollisionDataProvider
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DStaticMesh(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DStaticMesh() override;
#if DURIN_WITH_EDITORONLY_DATA
		// Rebuild the current accepted Source. Both modes capture owner-thread snapshots.
		// Synchronous mode returns the build result; asynchronous mode reports admission.
		// Query object diagnostics or the compiling manager for asynchronous completion.
		ENGINE_API auto Build(EStaticMeshBuildMode Mode, FStaticMeshBuildOptions Options = {})
			-> std::expected<void, std::string>;
		// Authoring transaction: candidate source/slots replace live data only on success.
		ENGINE_API auto BuildFromSource(EStaticMeshBuildMode Mode, FStaticMeshCompilationRequest Request)
			-> std::expected<void, std::string>;
#endif

#if DURIN_WITH_EDITORONLY_DATA
		// Owner-thread authored import commit. Validates before retiring prior render/collision
		// work, then installs source/slots/provenance and invalidates old projections together.
		ENGINE_API auto SetImportedSource(FStaticMeshSource Source,
			std::vector<FMeshMaterialSlotDefinition> Slots, DAssetImportData& ImportData)
			-> std::expected<void, std::string>;
#endif
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
		// Render compilation readiness; collision and GPU readiness remain independent.
		ENGINE_API auto IsCompiling() const -> bool;
		ENGINE_API auto GetCompilationDiagnostic() const -> FStaticMeshCompilationDiagnostic;
		ENGINE_API auto FinishCompilation()
			-> FAssetCompileProcessResult;
		ENGINE_API auto CancelCompilation() -> void;
		// Cancels and drains both render and collision work before authored mutation.
		ENGINE_API auto BlockOnAnyAsyncBuild() -> void;

		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		ENGINE_API auto SerializeCooked(FArchive& Ar) -> void override;
		ENGINE_API auto GetRenderData() const -> const FStaticMeshRenderData*;
		// Starts or joins bounded cooked loading and returns immediately with the
		// current generation-qualified CPU/GPU snapshot. Does not retry failed/cancelled work.
		ENGINE_API auto RequestRenderDataAndResources() -> FCookedMeshLoadStatus;
		// Blocks for CPU data only, retrying a prior CPU failure/cancellation.
		// May perform package I/O on the GameThread; use RequestRenderDataAndResources() for polling.
		// Does not request GPU initialization; prior rendering requests may still complete.
		ENGINE_API auto EnsureRenderDataLoadedBlocking()
			-> FCookedMeshBlockingResult;
		// Returns one coherent, nonblocking snapshot for stale-work rejection.
		ENGINE_API auto GetRenderResourceStatus() const
			-> FStaticMeshRenderResourceStatus;
		// Nonblocking query for queued GPU initialization only. False also covers
		// CPU loading, absent resources, initialization failure, and released resources.
		auto HasPendingRenderResourceInitialization() const -> bool
		{
			return GetRenderResourceStatus().Readiness
				== EStaticMeshRenderResourceReadiness::Queued;
		}
		// Returns finite, ordered CPU LOD 0 bounds, including zero-thickness bounds.
		ENGINE_API auto GetLOD0LocalBounds() const -> std::optional<FBox>;
		// Returns CPU LOD 0 bounds only when every axis has positive extent.
		ENGINE_API auto GetLOD0VolumetricBounds() const -> std::optional<FBox>;
		ENGINE_API auto ContainsPhysicsTriMeshData() const -> bool override;
		ENGINE_API auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, FPhysicsCookFailure> override;
		ENGINE_API auto CreatePhysicsMeshInputTask() const
			-> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, FPhysicsCookFailure> override;
		ENGINE_API auto GetBodySetup() const -> DBodySetup*;
		ENGINE_API auto NotifyCollisionDataChanged() -> void;
		ENGINE_API auto SetBodySetup(DBodySetup* InBodySetup) -> bool;
		// Accepts declared enum values on the owner thread, invalidates old collision and
		// schedules detached work from canonical source or procedural CPU values.
		// Build failure retains the new configuration with no derived collision.
		ENGINE_API auto SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> void;
		ENGINE_API auto SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> void;
		// Explicit retry; missing collision source is reported independently of render work.
		ENGINE_API auto RebuildCollision() -> void;
		// Owner notification after BodySetup invalidation; absent collision source is allowed.
		ENGINE_API auto NotifyCollisionSettingsChanged() -> void;
		ENGINE_API auto GetCollisionBuildStatus() const -> EPhysicsMeshBuildStatus;
		// Last collision build error, including admission and worker failure.
		ENGINE_API auto GetCollisionBuildError() const -> const std::optional<FPhysicsCookFailure>&;
		// Creates the qualified built-in Box setup from verified CPU bounds; arbitrary meshes remain collision-free.
		ENGINE_API auto EnsureQualifiedBoxBodySetup() -> DBodySetup*;
		// Queues GPU initialization for resident CPU data; query GetRenderResourceStatus()
		// for readiness. Explicit calls retry failed GPU initialization without reloading CPU data.
		ENGINE_API auto InitResources() -> void;
		auto GetAssetImportData() const -> const DAssetImportData*
		{
#if DURIN_WITH_EDITORONLY_DATA
			return AssetImportData.Get();
#else
			return nullptr;
#endif
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
#if DURIN_WITH_EDITORONLY_DATA
			return AssetImportData.Get();
#else
			return nullptr;
#endif
		}
		auto GetNumMaterialSlots() const -> uint32 { return static_cast<uint32>(MaterialSlots.size()); }
		auto GetMaterialSlots() const -> std::span<const FMeshMaterialSlotDefinition> { return MaterialSlots; }
		ENGINE_API auto GetMaterialSlot(uint32 SlotIndex) const -> const FMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(FName Name) const -> const FMeshMaterialSlotDefinition*;
		ENGINE_API auto GetMaterialIndex(FName Name) const -> std::optional<uint32>;
		ENGINE_API auto RenameMaterialSlot(uint32 SlotIndex, FName Name) -> std::expected<void, FStaticMeshSlotRenameError>;

		// Owner-thread observation only; never submits or retries loading.
		ENGINE_API auto GetRenderDataLoadStatus() const -> FCookedMeshLoadStatus;
#if DURIN_WITH_EDITORONLY_DATA
		auto GetSource() const -> const FStaticMeshSource& { return Source; }
#endif
		auto GetNormalizedSize() const -> float { return NormalizedSize; }
		auto GetCookedRenderData() const -> const FBulkData& { return CookedRenderData; }
		auto GetCookedCollisionData() const -> const FBulkData& { return CookedCollisionData; }
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostLoad() -> void override;
	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&) -> FCookContributionResult;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath) -> FCookContributionResult;
	public:

		ENGINE_API static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		// Requires an existing slot index on the owner thread. Null clears its default.
		ENGINE_API auto SetMaterialSlotDefaultMaterial(
			uint32 SlotIndex, DMaterialInterface* Material) -> void;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto IsReadyForFinishDestroy() -> bool override;
		ENGINE_API auto FinishDestroy() -> void override;

	private:
		friend class FStaticMeshTestAccess;
		// Owner-thread replacement invalidates old render/collision data first. Failure
		// leaves CPU data unavailable and returns a diagnostic; no rollback is performed.
		// Collision failure alone leaves the new CPU render data usable.
		// Does not change source metadata or dirty the package. GPU readiness is separate.
		ENGINE_API auto ReplaceRenderDataDestructively(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition> InMaterialSlots) -> std::expected<void, std::string>;
		// Also installs valid source settings before rebuilding, retaining them on failure.
		// Package dirtying remains the operation owner's responsibility.
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto ReplaceSourceRenderDataDestructively(
			FStaticMeshSource InSource,
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
			float InNormalizedSize) -> std::expected<void, std::string>;
#endif
		enum class EStaticMeshRenderResourceState : uint8
		{
			Uninitialized,
			InitializationQueued,
			Ready,
			Failed,
			ReleaseQueued,
			Released
		};

		static constexpr uint64 RenderResourceStateBits = 3;
		static constexpr uint64 RenderResourceStateMask =
			(1ull << RenderResourceStateBits) - 1;
		static constexpr auto PackRenderResourceStatus(
			EStaticMeshRenderResourceState State,
			uint64 Revision) -> uint64
		{
			return (Revision << RenderResourceStateBits)
				| static_cast<uint64>(State);
		}
		static constexpr auto UnpackRenderResourceState(uint64 Packed)
			-> EStaticMeshRenderResourceState
		{
			return static_cast<EStaticMeshRenderResourceState>(
				Packed & RenderResourceStateMask);
		}
		static constexpr auto UnpackRenderResourceRevision(uint64 Packed)
			-> uint64
		{
			return Packed >> RenderResourceStateBits;
		}
		auto LoadRenderResourceState() const
			-> EStaticMeshRenderResourceState;
		auto PublishRenderResourceState(EStaticMeshRenderResourceState State)
			-> void;
		auto TryPublishRenderResourceState(
			EStaticMeshRenderResourceState Expected,
			EStaticMeshRenderResourceState State) -> bool;
		auto AdvanceRenderResourceRevision() -> void;
		auto ReleaseResources() -> void;
		auto InvalidateRenderData() -> void;
		auto ValidateAndReplaceRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition> InMaterialSlots) -> std::expected<void, std::string>;
		auto InvalidateCollisionData() -> void;
		auto ScheduleCollisionData(bool bAllowUnavailable, bool bPersistDerivedData = true) -> void;
		auto RebuildCollisionData(bool bAllowUnavailable, bool bPersistDerivedData = true) -> void;
		auto RefreshCollisionBodies() -> void;
		auto CommitPreparedMeshData(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition>*
				InMaterialSlots,
			FStaticMeshSource* PreparedSource = nullptr,
			DAssetImportData* PreparedImportData = nullptr, bool bAcceptGeometry = true) -> std::expected<void, std::string>;
		friend ENGINE_API auto CommitStaticMeshBuild(DStaticMesh&, std::unique_ptr<FStaticMeshRenderData>, FStaticMeshSource,
			const FStaticMeshReconciliationSnapshot&, bool,
			DAssetImportData*, std::vector<FMeshMaterialSlotDefinition>*, bool) -> std::expected<void, std::string>;
		friend ENGINE_API auto PublishStaticMeshRenderData(DStaticMesh&, std::unique_ptr<FStaticMeshRenderData>) -> std::expected<void, std::string>;
		friend class FStaticMeshCompilingManager;
		auto LoadCookedRenderData() -> FCookedMeshLoadResult;
		auto SubmitCookedRenderDataRequest(bool bInitializeResources) -> bool;
		auto RefreshQualifiedBoxBodySetup() -> void;


#if DURIN_WITH_EDITORONLY_DATA
		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FStaticMeshSource Source;
#endif

		DPROPERTY()
		float NormalizedSize = 1.5f;

		DPROPERTY()
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;

		DPROPERTY()
		TObjectPtr<DBodySetup> BodySetup;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		float AcceptedGeometryNormalizedSize = 0;

		FCookedMeshLoadError CookedLoadError;
		FBulkData CookedRenderData;
		FBulkData CookedCollisionData;
		FRenderCommandFence ReleaseResourcesFence;
		std::atomic<uint64> RenderResourceStatus{PackRenderResourceStatus(
			EStaticMeshRenderResourceState::Uninitialized, 1)};
		std::atomic<ECookedMeshCpuPhase> CookedLoadPhase{ECookedMeshCpuPhase::Unloaded};
		std::atomic<uint64> CookedLoadGeneration{1};

	};

}
