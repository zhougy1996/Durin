#pragma once
#include <expected>

#include "Components/PrimitiveComponent.h"
#include "DObject/ObjectPtr.h"
#include "StaticMesh/StaticMeshMaterialBinding.h"

#include <optional>
#include <span>
#include <vector>

#include "MeshComponent.gen.h"

namespace Durin
{
	class DMaterialInterface;

	// Owns positional material overrides independently of the backing mesh geometry.
	DCLASS()
	class DMeshComponent : public DPrimitiveComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API virtual auto GetNumMaterials() const -> uint32;
		ENGINE_API virtual auto GetMaterialIndex(FName SlotName) const -> std::optional<uint32>;
		ENGINE_API virtual auto GetDefaultMaterial(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API auto GetMaterial() const -> DMaterialInterface*;
		ENGINE_API virtual auto GetMaterial(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API auto SetMaterial(DMaterialInterface* InMaterial) -> bool;
		// A null material clears the override; unavailable slots reject mutation.
		ENGINE_API virtual auto SetMaterial(uint32 SlotIndex, DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto SetMaterialByName(FName SlotName, DMaterialInterface* InMaterial) -> bool;
		ENGINE_API auto GetMaterialByName(FName SlotName) const -> DMaterialInterface*;
		// Reset returns whether an override changed; Set also succeeds for unchanged valid slots.
		ENGINE_API auto ResetMaterial(uint32 SlotIndex) -> bool;
		ENGINE_API auto ClearMaterialOverrides() -> bool;
		ENGINE_API auto GetMaterialOverride(uint32 SlotIndex) const -> DMaterialInterface*;
		ENGINE_API auto HasMaterialOverride(uint32 SlotIndex) const -> bool;
		auto GetOverrideMaterials() const -> std::span<const TObjectPtr<DMaterialInterface>> { return OverrideMaterials; }
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostLoad() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

	private:
		ENGINE_API auto BuildMaterialRenderProxyBindingUpdate(
			FMaterialRenderProxyBindingUpdate& OutUpdate) -> bool override;
		auto ValidateOverrideMaterials(std::span<const TObjectPtr<DMaterialInterface>> Overrides) const -> FStaticMeshMaterialOverrideResult;

		// Keep dormant indices across mesh replacement; only non-null overrides are authored.
		DPROPERTY()
		std::vector<TObjectPtr<DMaterialInterface>> OverrideMaterials;

		uint32 PendingMaterialSlotIndex = 0;
	};
}
