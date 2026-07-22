#pragma once

#include "Components/MeshComponent.h"

#include "StaticMeshComponent.gen.h"

namespace Durin
{
	class DStaticMesh;
	class DMaterialInterface;

	DCLASS()
	class DStaticMeshComponent : public DMeshComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto SetStaticMesh(DStaticMesh* InStaticMesh) -> void;
		ENGINE_API auto GetStaticMesh() const -> DStaticMesh*;
		ENGINE_API auto SetMaterial(DMaterialInterface* InMaterial) -> void;
		ENGINE_API auto GetMaterial() const -> DMaterialInterface*;
		ENGINE_API auto SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> void;
		ENGINE_API auto GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API auto GetNumMaterials() const -> uint32;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy> override;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		auto BuildMaterialRenderUpdate(EMaterialRenderDirtyFlags DirtyFlags, FMaterialRenderUpdate& OutUpdate) -> bool override;
		auto HandleMaterialRenderDataChanged(DMaterialInterface* ChangedMaterial, EMaterialRenderDirtyFlags DirtyFlags) -> void;
		auto BindMaterial(DMaterialInterface* InMaterial) -> void;
		auto UnbindMaterial(DMaterialInterface* InMaterial) -> void;
		auto ReconcileMaterialBindings() -> void;

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> StaticMesh;

		// Retained as a serialized slot-zero mirror so existing component assets keep their material.
		DPROPERTY()
		TObjectPtr<DMaterialInterface> Material;

		DPROPERTY(Edit)
		std::vector<TObjectPtr<DMaterialInterface>> Materials;
		std::vector<TObjectPtr<DMaterialInterface>> BoundMaterials;

		uint64 MaterialComponentRevision = 1;
		uint32 PendingMaterialSlotIndex = 0;
		EMaterialRenderDirtyFlags PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::None;

		friend class DMaterialInterface;
	};
}
