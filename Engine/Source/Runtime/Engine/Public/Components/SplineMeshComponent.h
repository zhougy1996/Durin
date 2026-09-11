#pragma once

#include "Components/MeshComponent.h"
#include "Collision/CollisionGeometry.h"
#include "Spline/SplineTypes.h"
#include "StaticMesh/StaticMeshResources.h"

#include "SplineMeshComponent.gen.h"

namespace Durin
{
	class DMaterialInterface;
	class DStaticMesh;
	class FStaticMeshRenderStateRecreateContext;

	enum class ESplineMeshDerivedStateStatus : uint8
	{
		Valid,
		NoStaticMesh,
		SourceDataUnavailable,
		InvalidSourceData,
		InvalidDeformation
	};

	DENUM()
	enum class ESplineMeshCollisionMode : uint8
	{
		Disabled,
		DeformedTriangleMesh
	};

	// Immutable deformation snapshot. Exact CPU geometry is populated only on demand.
	struct FSplineMeshDerivedState
	{
		FSplineMeshParams Params;
		FBox ConservativeLocalBounds;
		// Empty in a valid render-only snapshot; request GetDerivedStateForQueries for exact geometry.
		std::vector<FVector3f> DeformedLOD0Positions;
		std::vector<uint32> LOD0Indices;
		std::shared_ptr<const FStaticMeshLODResources::FRayQueryAcceleration> EditorAcceleration;
		uint64 SourceRenderResourceRevision = 0;
		uint64 DeformationRevision = 0;
		uint64 CollisionInputIdentity = 0;
		FCollisionGeometryRef CollisionGeometry;
		ESplineMeshDerivedStateStatus Status = ESplineMeshDerivedStateStatus::NoStaticMesh;
		std::string Diagnostic;

		auto IsValid() const -> bool { return Status == ESplineMeshDerivedStateStatus::Valid; }
	};

	// Deforms a borrowed StaticMesh through a value-only Hermite interval.
	DCLASS()
	class DSplineMeshComponent : public DMeshComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSplineMeshComponent(const FObjectInitializer& ObjectInitializer);

		// Deferred setters change authored values only. Call UpdateMesh before leaving the batch;
		// registration flushes pending edits. Existing render/physics snapshots remain published meanwhile.
		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh, bool bUpdateMesh = true) -> void;
		auto GetStaticMesh() const -> DStaticMesh* { return StaticMesh.Get(); }
		// Stores authored parameters; validation and normalization happen during UpdateMesh.
		ENGINE_API auto SetSplineMeshParams(const FSplineMeshParams& InParams, bool bUpdateMesh = true) -> void;
		auto GetSplineMeshParams() const -> const FSplineMeshParams& { return SplineMeshParams; }
		auto GetDeformationRevision() const -> uint64 { return DeformationRevision; }
		auto GetSplineMeshCollisionMode() const -> ESplineMeshCollisionMode { return CollisionMode; }
		ENGINE_API auto SetSplineMeshCollisionMode(ESplineMeshCollisionMode InMode, bool bUpdateMesh = true) -> void;
		// Applies pending edits, invalidates failed results, and logs diagnostics internally.
		// Also retries a failed update; clean successful updates do nothing.
		ENGINE_API auto UpdateMesh() -> void;
		auto GetMeshUpdateError() const -> const std::string& { return MeshUpdateError; }
		auto GetCollisionBuildError() const -> const std::string& { return CollisionBuildError; }
		auto GetQueryBuildError() const -> const std::string& { return QueryBuildError; }
		auto IsMeshDirty() const -> bool { return bSourceDirty || bDeformationDirty || bCollisionDirty; }
		// Reads the published snapshot without building CPU geometry. IsValid describes render readiness.
		ENGINE_API auto GetDerivedState() const -> std::shared_ptr<const FSplineMeshDerivedState>;
		// Call on the component's owning thread. Builds and caches exact geometry for this revision;
		// Returns null on failure or when uncached geometry belongs to a pending source replacement,
		// without changing the published render state.
		ENGINE_API auto GetDerivedStateForQueries() -> std::shared_ptr<const FSplineMeshDerivedState>;
		ENGINE_API auto BuildCollisionGeometry(
			FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool override;

		ENGINE_API auto SetMaterial(DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool override;
		ENGINE_API auto GetMaterial(uint32 SlotIndex = 0) const -> DMaterialInterface* override;
		ENGINE_API auto ResetMaterial(uint32 SlotIndex) -> bool;
		ENGINE_API auto ClearMaterialOverrides() -> bool;
		auto GetOverrideMaterials() const -> std::span<const TObjectPtr<DMaterialInterface>> { return OverrideMaterials; }
		ENGINE_API auto GetNumMaterials() const -> uint32 override;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;
		ENGINE_API auto OnRegister() -> void override;

		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
#if DURIN_WITH_EDITOR
		ENGINE_API auto GetEditorPickingLocalBounds(FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool override;
#endif

	private:
		friend class FStaticMeshRenderStateRecreateContext;
		ENGINE_API auto BuildMaterialRenderProxyBindingUpdate(
			FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool override;
		auto RebuildDerivedState(std::string* OutError = nullptr) -> bool;
		auto BuildDerivedGeometry(FSplineMeshDerivedState& Candidate, std::string* OutError) const -> bool;
		auto HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void;
		auto PushDynamicDataToScene() -> void;
		auto ValidateOverrideMaterials(std::span<const TObjectPtr<DMaterialInterface>> Overrides, std::string& OutError) const -> bool;
		auto GetMaterialOverride(uint32 SlotIndex) const -> DMaterialInterface*;
		auto GetCollisionStateRevision() const -> uint64 override;
		auto RebuildCollisionGeometryForPublishedState() -> void;

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> StaticMesh;

		DPROPERTY(Edit)
		FSplineMeshParams SplineMeshParams;

		DPROPERTY()
		std::vector<TObjectPtr<DMaterialInterface>> OverrideMaterials;

		DPROPERTY(Edit)
		ESplineMeshCollisionMode CollisionMode = ESplineMeshCollisionMode::Disabled;

		std::shared_ptr<const FSplineMeshDerivedState> DerivedState;
		uint64 DeformationRevision = 0;
		uint64 MaterialComponentRevision = 1;
		uint32 PendingMaterialSlotIndex = 0;
		bool bSourceDirty = false;
		bool bDeformationDirty = false;
		bool bCollisionDirty = false;
		// Resource requests can synchronously notify this component while UpdateMesh is rebuilding it.
		bool bUpdatingMesh = false;
		std::string MeshUpdateError;
		std::string CollisionBuildError;
		std::string QueryBuildError;
		// Physics reads the applied mode until deferred authored edits are applied.
		ESplineMeshCollisionMode PublishedCollisionMode = ESplineMeshCollisionMode::Disabled;
	};
}
