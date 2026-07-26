#pragma once

#include "Components/MeshComponent.h"

#include "StaticMeshComponent.gen.h"

namespace Durin
{
	class DStaticMesh;
	class DMaterialInterface;

	// Associates a component material override with a stable static-mesh slot identifier.
	DSTRUCT()
	struct FStaticMeshMaterialOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid SlotId;

		DPROPERTY()
		TObjectPtr<DMaterialInterface> Material;
	};

	// Binds a static mesh and per-slot materials to a render-scene primitive.
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
		ENGINE_API auto SetMaterialBySlotId(const FGuid& SlotId, DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto ResetMaterialBySlotId(const FGuid& SlotId) -> bool;
		ENGINE_API auto RemoveMaterialOverride(const FGuid& SlotId) -> bool;
		ENGINE_API auto GetMaterialBySlotId(const FGuid& SlotId) const -> DMaterialInterface*;
		ENGINE_API auto GetMaterialOverride(const FGuid& SlotId) const -> DMaterialInterface*;
		ENGINE_API auto HasMaterialOverride(const FGuid& SlotId) const -> bool;
		ENGINE_API auto IsMaterialOverrideOrphan(const FGuid& SlotId) const -> bool;
		ENGINE_API auto GetMaterialOverrides() const -> std::span<const FStaticMeshMaterialOverride> { return MaterialOverrides; }
		ENGINE_API auto GetNumMaterials() const -> uint32;
		ENGINE_API auto CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy> override;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		auto BuildMaterialRenderUpdate(EMaterialRenderDirtyFlags DirtyFlags, FMaterialRenderUpdate& OutUpdate) -> bool override;
		auto HandleMaterialRenderDataChanged(uint32 SlotIndex, EMaterialRenderDirtyFlags DirtyFlags) -> void;
		auto HandleStaticMeshRenderDataChanged(DStaticMesh* ChangedMesh) -> void;
		auto BindMaterial(DMaterialInterface* InMaterial) -> void;
		auto UnbindMaterial(DMaterialInterface* InMaterial) -> void;
		auto ReconcileStaticMeshBinding() -> void;
		auto ReconcileMaterialBindings() -> void;
		auto ValidateMaterialOverrides(std::span<const FStaticMeshMaterialOverride> Overrides, std::string& OutError) const -> bool;

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> StaticMesh;

		DPROPERTY()
		std::vector<FStaticMeshMaterialOverride> MaterialOverrides;
		std::vector<TObjectPtr<DMaterialInterface>> BoundMaterials;
		DStaticMesh* BoundStaticMesh = nullptr;

		uint64 MaterialComponentRevision = 1;
		uint32 PendingMaterialSlotIndex = 0;
		EMaterialRenderDirtyFlags PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::None;

		friend class DMaterialInterface;
		friend class DStaticMesh;
		friend class FMaterialUpdateContext;
	};
}
