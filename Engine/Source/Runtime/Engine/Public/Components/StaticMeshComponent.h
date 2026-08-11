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
		ENGINE_API auto GetBodySetup() const -> DBodySetup*;
		ENGINE_API auto BuildCollisionShape(FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool override;
		ENGINE_API auto SetMaterial(DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto GetMaterial() const -> DMaterialInterface*;
		ENGINE_API auto SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API auto SetMaterialByName(FName SlotName, DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto GetMaterialByName(FName SlotName) const -> DMaterialInterface*;
		ENGINE_API auto ResetMaterial(uint32 SlotIndex) -> bool;
		ENGINE_API auto ClearMaterialOverrides() -> bool;
		ENGINE_API auto GetMaterialOverride(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API auto HasMaterialOverride(uint32 SlotIndex) const -> bool;
		auto GetOverrideMaterials() const -> std::span<const TObjectPtr<DMaterialInterface>> { return OverrideMaterials; }
		ENGINE_API auto GetNumMaterials() const -> uint32;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<FPrimitiveSceneProxy> override;
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
		auto HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void;
		auto ValidateOverrideMaterials(std::span<const TObjectPtr<DMaterialInterface>> Overrides, std::string& OutError) const -> bool;
		auto TrimTrailingNullOverrides() -> void;

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> StaticMesh;

		DPROPERTY()
		std::vector<TObjectPtr<DMaterialInterface>> OverrideMaterials;

		uint64 MaterialComponentRevision = 1;
		uint32 PendingMaterialSlotIndex = 0;

		friend class DStaticMesh;
	};
}
