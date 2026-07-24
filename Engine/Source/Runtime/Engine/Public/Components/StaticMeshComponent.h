#pragma once

#include "Components/MeshComponent.h"

#include "StaticMeshComponent.gen.h"

namespace Durin
{
	class DStaticMesh;
	class DMaterialInterface;
	class FArchive;

	DSTRUCT()
	struct ENGINE_API FStaticMeshMaterialOverride
	{
		GENERATED_BODY()

		DPROPERTY()
		FGuid SlotId;

		DPROPERTY()
		TObjectPtr<DMaterialInterface> Material;
	};

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
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
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
		auto ValidateMaterialOverrides(std::span<const FStaticMeshMaterialOverride> Overrides, std::string& OutError) const -> bool;
		auto MigrateLegacyMaterials() -> void;

		DPROPERTY(Edit)
		TObjectPtr<DStaticMesh> StaticMesh;

		DPROPERTY()
		uint32 MaterialOverridesVersion = 0;

		DPROPERTY()
		std::vector<FStaticMeshMaterialOverride> MaterialOverrides;

		// Transitional version-zero migration inputs. New saves clear both fields.
		DPROPERTY()
		TObjectPtr<DMaterialInterface> Material;

		DPROPERTY()
		std::vector<TObjectPtr<DMaterialInterface>> Materials;
		std::vector<TObjectPtr<DMaterialInterface>> BoundMaterials;

		uint64 MaterialComponentRevision = 1;
		uint32 PendingMaterialSlotIndex = 0;
		EMaterialRenderDirtyFlags PendingMaterialDirtyFlags = EMaterialRenderDirtyFlags::None;

		friend class DMaterialInterface;
	};
}
