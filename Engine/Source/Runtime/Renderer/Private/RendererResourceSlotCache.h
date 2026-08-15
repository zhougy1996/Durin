#pragma once

#include "RenderResourceCreation.h"

#include <algorithm>
#include <cstddef>
#include <limits>
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
				NextIndex++,
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

		template <typename WeightFunction>
		auto GetRetainedPayloadWeight(WeightFunction&& GetWeight) const -> uint64
		{
			uint64 Total = 0;
			for (const FEntry& Entry : Entries)
			{
				const PayloadType* Payload = Entry.Slot.GetPayload();
				if (Payload != nullptr)
				{
					const uint64 Weight =
						static_cast<uint64>(GetWeight(Entry.Key, *Payload));
					Total = Weight > std::numeric_limits<uint64>::max() - Total
						? std::numeric_limits<uint64>::max()
						: Total + Weight;
				}
			}
			return Total;
		}

		auto EvictOldestExcept(const KeyType& RetainedKey) -> bool
		{
			auto Oldest = Entries.end();
			for (auto It = Entries.begin(); It != Entries.end(); ++It)
			{
				if (It->Key != RetainedKey
					&& (Oldest == Entries.end() || It->Index < Oldest->Index))
				{
					Oldest = It;
				}
			}
			if (Oldest == Entries.end()) return false;
			Entries.erase(Oldest);
			return true;
		}

		auto Reset() -> void
		{
			Entries.clear();
			NextIndex = 0;
		}

	private:
		ERenderResourceGenerationDependency PayloadDependencies;
		std::vector<FEntry> Entries;
		size_t NextIndex = 0;
	};
}
