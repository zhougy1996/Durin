#pragma once

#include "RenderResourceCreation.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace Durin
{
	// Keeps keyed renderer slots in insertion order. Entry references are valid
	// only until the next insertion; payloads are returned by value/reference
	// ownership at the call site and never expose a long-lived slot pointer.
	template <typename KeyType, typename PayloadType>
	class TRendererResourceSlotCache
	{
	public:
		struct FEntry
		{
			FEntry(
				KeyType InKey,
				size_t InIndex,
				ERenderResourceGenerationDependency Dependencies)
				: Key(std::move(InKey))
				, Index(InIndex)
				, Slot(Dependencies)
			{
			}

			KeyType Key;
			size_t Index = 0;
			TRenderResourceCreationSlot<PayloadType> Slot;
		};

		explicit TRendererResourceSlotCache(
			ERenderResourceGenerationDependency InPayloadDependencies =
				ERenderResourceGenerationDependency::None)
			: PayloadDependencies(InPayloadDependencies)
		{
		}

		auto FindOrAdd(const KeyType& Key) -> FEntry&
		{
			const auto Existing = std::ranges::find(
				Entries,
				Key,
				&FEntry::Key);
			if (Existing != Entries.end())
			{
				return *Existing;
			}
			return Entries.emplace_back(
				Key,
				Entries.size(),
				PayloadDependencies);
		}

		auto Find(const KeyType& Key) -> FEntry*
		{
			const auto Existing = std::ranges::find(
				Entries,
				Key,
				&FEntry::Key);
			return Existing != Entries.end() ? &*Existing : nullptr;
		}

		auto Find(const KeyType& Key) const -> const FEntry*
		{
			const auto Existing = std::ranges::find(
				Entries,
				Key,
				&FEntry::Key);
			return Existing != Entries.end() ? &*Existing : nullptr;
		}

		auto Num() const -> size_t { return Entries.size(); }

		auto Reset() -> void { Entries.clear(); }

	private:
		ERenderResourceGenerationDependency PayloadDependencies;
		std::vector<FEntry> Entries;
	};
}
