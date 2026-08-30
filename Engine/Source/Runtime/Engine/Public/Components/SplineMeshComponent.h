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
		InvalidSourceData
	};

	DENUM()
	enum class ESplineMeshCollisionMode : uint8
	{
		Disabled,
		DeformedTriangleMesh
	};

	// Immutable CPU authority used by bounds, exact editor queries, collision, and shader parity.
	struct FSplineMeshDerivedState
	{
		FSplineMeshParams Params;
		FBox ConservativeLocalBounds;
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

		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh) -> void;
		auto GetStaticMesh() const -> DStaticMesh* { return StaticMesh.Get(); }
		ENGINE_API auto SetSplineMeshParams(const FSplineMeshParams& InParams, std::string* OutError = nullptr) -> bool;
		auto GetSplineMeshParams() const -> const FSplineMeshParams& { return SplineMeshParams; }
		auto GetDeformationRevision() const -> uint64 { return DeformationRevision; }
		auto GetSplineMeshCollisionMode() const -> ESplineMeshCollisionMode { return CollisionMode; }
		ENGINE_API auto SetSplineMeshCollisionMode(ESplineMeshCollisionMode InMode) -> void;
		ENGINE_API auto GetDerivedState() const -> std::shared_ptr<const FSplineMeshDerivedState>;
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

		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
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
	};
}
