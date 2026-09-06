#pragma once

#include "Asset/CookedMeshLoading.h"

#include "Asset/AssetImportData.h"
#include "Asset/BulkData.h"
#include "Asset/Cook.h"
#include "StaticMesh/StaticMeshSource.h"
#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "DObject/ObjectPtr.h"
#include "Materials/MeshMaterialSlot.h"
#include "RenderingThread.h"

#include "StaticMesh.gen.h"

namespace Durin
{

	class DBodySetup;
	class FStaticMeshAuthoredCandidate;
	struct FStaticMeshReconciliationSnapshot;
	struct FStaticMeshBuildExecutionControl;
	struct FStaticMeshBuildOutcome;
	class FCollisionGeometryRef;
	enum class EBodySetupCollisionSourceMode : uint8;
	enum class EBodySetupCollisionQueryPolicy : uint8;
	enum class ECollisionGeometryKind : uint8;

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

	// Selects a signed source axis when converting imported geometry to Durin space.
	DENUM()
	enum class EStaticMeshImportAxis : int8
	{
		PositiveX,
		NegativeX,
		PositiveY,
		NegativeY,
		PositiveZ,
		NegativeZ
	};

	// Defines the orthogonal source basis used during static-mesh import.
	DSTRUCT()
	struct FStaticMeshImportSettings
	{
		GENERATED_BODY()

		DPROPERTY()
		EStaticMeshImportAxis ForwardAxis = EStaticMeshImportAxis::PositiveX;

		DPROPERTY()
		EStaticMeshImportAxis RightAxis = EStaticMeshImportAxis::PositiveY;

		DPROPERTY()
		EStaticMeshImportAxis UpAxis = EStaticMeshImportAxis::PositiveZ;

		ENGINE_API auto IsValid(std::string* OutError = nullptr) const -> bool;

		ENGINE_API static auto MakeDurin() -> FStaticMeshImportSettings;
		ENGINE_API static auto MakeYUpNegativeZForward() -> FStaticMeshImportSettings;

		auto operator==(const FStaticMeshImportSettings&) const -> bool = default;
	};

	struct FStaticMeshBuildData;
	struct FStaticMeshRenderData;

	// Owns imported mesh metadata, material slots, and rebuilt render resources.
	DCLASS()
	class DStaticMesh : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DStaticMesh(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DStaticMesh() override;
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
		ENGINE_API auto GetBodySetup() const -> DBodySetup*;
		ENGINE_API auto SetBodySetup(DBodySetup* InBodySetup) -> bool;
		ENGINE_API auto SetCollisionSourceMode(
			EBodySetupCollisionSourceMode Mode,
			std::string& OutError) -> bool;
		ENGINE_API auto SetCollisionQueryPolicy(
			EBodySetupCollisionQueryPolicy Policy,
			std::string& OutError) -> bool;
		ENGINE_API auto RebuildCollision(std::string& OutError) -> bool;
		// Creates the qualified built-in Box setup from verified CPU bounds; arbitrary meshes remain collision-free.
		ENGINE_API auto EnsureQualifiedBoxBodySetup() -> DBodySetup*;
		// Queues GPU initialization for resident CPU data; query GetRenderResourceStatus()
		// for readiness. Explicit calls retry failed GPU initialization without reloading CPU data.
		ENGINE_API auto InitResources() -> void;
		auto GetAssetImportData() const -> const DAssetImportData*
		{
			return AssetImportData.Get();
		}
		auto GetAssetImportData() -> DAssetImportData*
		{
			return AssetImportData.Get();
		}
		ENGINE_API auto PublishAssetImportData(
			DAssetImportData& Value, std::string& OutError) -> bool;
		auto GetNumMaterialSlots() const -> uint32 { return static_cast<uint32>(MaterialSlots.size()); }
		auto GetMaterialSlots() const -> std::span<const FMeshMaterialSlotDefinition> { return MaterialSlots; }
		ENGINE_API auto GetMaterialSlot(uint32 SlotIndex) const -> const FMeshMaterialSlotDefinition*;
		ENGINE_API auto FindMaterialSlot(FName Name) const -> const FMeshMaterialSlotDefinition*;
		ENGINE_API auto GetMaterialIndex(FName Name) const -> std::optional<uint32>;
		ENGINE_API auto RenameMaterialSlot(uint32 SlotIndex, FName Name, std::string& OutError) -> bool;

		// Owner-thread observation only; never submits or retries loading.
		ENGINE_API auto GetRenderDataLoadStatus() const -> FCookedMeshLoadStatus;
		auto GetImportedData() const -> const FStaticMeshImportedData& { return ImportedData; }
		auto GetNormalizedSize() const -> float { return NormalizedSize; }
		auto GetCookedRenderData() const -> const FBulkData& { return CookedRenderData; }
		auto GetCookedCollisionData() const -> const FBulkData& { return CookedCollisionData; }
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
	private:
		friend auto ::Durin::ContributeEngineCookAsset(
			DObject&, std::string_view, FCookContext&, std::string&) -> bool;
		ENGINE_API auto ContributeToCook(
			FCookContext& Context,
			std::string_view VirtualPackagePath,
			std::string& OutError) -> bool;
	public:

		ENGINE_API static auto CreateDebugTriangle(DObject* Outer = nullptr) -> DStaticMesh*;
		// Installs validated CPU values with rollback-safe render/collision replacement.
		// Authored inputs and package dirty state are unchanged.
		ENGINE_API auto SetRenderData(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
			std::string& OutError) -> bool;
		// Validates detached values before atomic render/collision replacement.
		// Does not dirty the package or retain build-operation diagnostics.
		ENGINE_API auto SetImportedRenderData(
			FStaticMeshImportedData InImportedData,
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
			float InNormalizedSize,
			std::string& OutError) -> bool;
		ENGINE_API auto SetImportedDefaultMaterial(
			uint32 SourceMaterialIndex,
			DMaterialInterface* Material,
			std::string& OutError) -> bool;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto IsReadyForFinishDestroy() -> bool override;
		ENGINE_API auto FinishDestroy() -> void override;

	private:
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
		auto CommitRenderDataCandidate(
			std::unique_ptr<FStaticMeshRenderData> InRenderData,
			std::vector<FMeshMaterialSlotDefinition>*
				InMaterialSlots,
			std::string& OutError,
			bool bBuildAuthoredCollision = true,
			FStaticMeshAuthoredCandidate* AuthoredCandidate = nullptr,
			DAssetImportData* PreparedImportData = nullptr) -> bool;
		friend auto ApplyStaticMeshAuthoredCandidate(DStaticMesh&,
			std::unique_ptr<FStaticMeshAuthoredCandidate>, const FStaticMeshReconciliationSnapshot&,
			std::string&, bool, const FStaticMeshBuildExecutionControl&, DAssetImportData*) -> FStaticMeshBuildOutcome;
		auto LoadCookedRenderData(std::string& OutError) -> bool;
		auto SubmitCookedRenderDataRequest(bool bInitializeResources) -> bool;
		auto RefreshQualifiedBoxBodySetup() -> void;
		auto BuildCollisionCandidate(
			const FStaticMeshRenderData& SourceRenderData,
			EBodySetupCollisionSourceMode Mode,
			EBodySetupCollisionQueryPolicy Policy,
			FCollisionGeometryRef& OutSimple,
			FCollisionGeometryRef& OutComplex,
			std::string& OutError) const -> bool;

		DPROPERTY(EditorOnly)
		TObjectPtr<DAssetImportData> AssetImportData;

		DPROPERTY(EditorOnly)
		FStaticMeshImportedData ImportedData;

		DPROPERTY()
		float NormalizedSize = 1.5f;

		DPROPERTY()
		std::vector<FMeshMaterialSlotDefinition> MaterialSlots;

		DPROPERTY()
		TObjectPtr<DBodySetup> BodySetup;

		std::unique_ptr<FStaticMeshRenderData> RenderData;
		FBulkData CookedRenderData;
		FBulkData CookedCollisionData;
		FRenderCommandFence ReleaseResourcesFence;
		std::atomic<uint64> RenderResourceStatus{PackRenderResourceStatus(
			EStaticMeshRenderResourceState::Uninitialized, 1)};
		std::atomic<ECookedMeshCpuPhase> CookedLoadPhase{ECookedMeshCpuPhase::Unloaded};
		std::atomic<uint64> CookedLoadGeneration{1};

	};

}
