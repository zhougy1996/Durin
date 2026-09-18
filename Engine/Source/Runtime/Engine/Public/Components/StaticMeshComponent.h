#pragma once

#include "Components/MeshComponent.h"

#include "StaticMeshComponent.gen.h"

namespace Durin
{
	class DStaticMesh;
	class DMaterialInterface;
	class DBodySetup;

	// Binds a static mesh and per-slot materials to a render-scene primitive.
	DCLASS()
	class DStaticMeshComponent : public DMeshComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DStaticMeshComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh) -> void;
		ENGINE_API auto GetStaticMesh() const -> DStaticMesh*;
		// Refreshes render and physics consumers after transactional reference replacement.
		ENGINE_API auto RefreshReloadedAssetBindings() -> void;
		ENGINE_API auto GetBodySetup() const -> DBodySetup*;
		ENGINE_API auto BuildCollisionShape(FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool override;
		ENGINE_API auto BuildCollisionGeometry(
			FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool override;
		ENGINE_API auto GetNumMaterials() const -> uint32 override;
		ENGINE_API auto GetMaterialIndex(FName SlotName) const -> std::optional<uint32> override;
		ENGINE_API auto GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface* override;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> FObjectValidationResult override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;
#if DURIN_WITH_EDITOR
		ENGINE_API auto GetEditorPickingLocalBounds(FBox& OutBounds, EEditorPickingPrimitiveFamily& OutFamily) const -> bool override;
#endif

	private:
		friend class FStaticMeshRenderStateRecreateContext;

		ENGINE_API auto GetCollisionStateRevision() const -> uint64 override;
		auto HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void;

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> StaticMesh;

		friend class DStaticMesh;
	};
}
