#pragma once

#include "DObject/ObjectPtr.h"
#include "Materials/MaterialRenderProxy.h"

namespace Durin
{
	class DMaterialInterface;

	namespace ComponentMaterialOverride
	{
		// Distinguishes invalid requests and idempotent calls from state-changing override mutations.
		enum class EMutationResult : uint8
		{
			InvalidSlot,
			Unchanged,
			Changed
		};

		// Mutates positional overrides and their render-binding revision without owning component lifecycle effects.
		auto Set(
			std::vector<TObjectPtr<DMaterialInterface>>& Overrides,
			uint32 SlotIndex,
			bool bSlotExists,
			DMaterialInterface* Material,
			uint64& Revision,
			uint32& PendingSlotIndex) -> EMutationResult;
		auto Clear(
			std::vector<TObjectPtr<DMaterialInterface>>& Overrides,
			uint64& Revision,
			uint32& PendingSlotIndex) -> bool;
		auto Get(
			std::span<const TObjectPtr<DMaterialInterface>> Overrides,
			uint32 SlotIndex) -> DMaterialInterface*;
		auto Resolve(
			std::span<const TObjectPtr<DMaterialInterface>> Overrides,
			uint32 SlotIndex,
			DMaterialInterface* DefaultMaterial) -> DMaterialInterface*;
		auto TrimTrailingNulls(
			std::vector<TObjectPtr<DMaterialInterface>>& Overrides) -> void;
		auto ResolveRenderProxy(DMaterialInterface* Material) -> FMaterialRenderProxyRef;
		auto BuildRenderProxyBindingUpdate(
			uint32 SlotIndex,
			DMaterialInterface* Material,
			uint64 Revision,
			FMaterialRenderProxyBindingUpdate& OutUpdate) -> void;
	}
}
