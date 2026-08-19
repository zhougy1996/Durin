#include "Components/ComponentMaterialOverride.h"

#include "Materials/DefaultMaterialService.h"
#include "Materials/MaterialInterface.h"

namespace Durin::ComponentMaterialOverride
{
	auto Set(
		std::vector<TObjectPtr<DMaterialInterface>>& Overrides,
		uint32 SlotIndex,
		bool bSlotExists,
		DMaterialInterface* Material,
		uint64& Revision,
		uint32& PendingSlotIndex) -> EMutationResult
	{
		if (!bSlotExists) return EMutationResult::InvalidSlot;
		if (Get(Overrides, SlotIndex) == Material) return EMutationResult::Unchanged;
		if (Material)
		{
			if (SlotIndex >= Overrides.size()) Overrides.resize(static_cast<size_t>(SlotIndex) + 1);
			Overrides[SlotIndex] = Material;
		}
		else
		{
			Overrides[SlotIndex] = nullptr;
			TrimTrailingNulls(Overrides);
		}
		++Revision;
		PendingSlotIndex = SlotIndex;
		return EMutationResult::Changed;
	}

	auto Clear(
		std::vector<TObjectPtr<DMaterialInterface>>& Overrides,
		uint64& Revision,
		uint32& PendingSlotIndex) -> bool
	{
		if (Overrides.empty()) return false;
		Overrides.clear();
		++Revision;
		PendingSlotIndex = 0;
		return true;
	}

	auto Get(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides,
		uint32 SlotIndex) -> DMaterialInterface*
	{
		return SlotIndex < Overrides.size() ? Overrides[SlotIndex].Get() : nullptr;
	}

	auto Resolve(
		std::span<const TObjectPtr<DMaterialInterface>> Overrides,
		uint32 SlotIndex,
		DMaterialInterface* DefaultMaterial) -> DMaterialInterface*
	{
		if (DMaterialInterface* Override = Get(Overrides, SlotIndex)) return Override;
		return DefaultMaterial;
	}

	auto TrimTrailingNulls(std::vector<TObjectPtr<DMaterialInterface>>& Overrides) -> void
	{
		while (!Overrides.empty() && !Overrides.back()) Overrides.pop_back();
	}

	auto ResolveRenderProxy(DMaterialInterface* Material) -> FMaterialRenderProxyRef
	{
		if (Material) return Material->GetMaterialRenderProxy();
		RecordMaterialFallbackReason(EMaterialFallbackReason::UnassignedDefault);
		return GetDefaultMaterialRenderProxy();
	}

	auto BuildRenderProxyBindingUpdate(
		uint32 SlotIndex,
		DMaterialInterface* Material,
		uint64 Revision,
		FMaterialRenderProxyBindingUpdate& OutUpdate) -> void
	{
		OutUpdate.SlotIndex = SlotIndex;
		OutUpdate.MaterialProxy = ResolveRenderProxy(Material);
		OutUpdate.ComponentRevision = Revision;
	}
}
