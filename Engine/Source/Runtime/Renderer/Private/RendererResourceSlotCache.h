#pragma once

#include "CoreMinimal.h"

#include "RenderResourceCreation.h"

namespace Durin
{
	// Render-thread-owned slots. Mutable lookup promotes recency; const lookup is
	// observational. References survive insertion, but not their eviction/reset.
	template <typename KeyType, typename PayloadType, typename Hasher = std::hash<KeyType>>
	class TRendererResourceSlotCache
	{
	public:
		struct FEntry
		{
			FEntry(KeyType InKey, size_t InIndex,
				ERenderResourceGenerationDependency Dependencies)
				: Key(std::move(InKey)), Index(InIndex), Slot(Dependencies) {}

			const KeyType Key;
			size_t Index = 0;
			TRenderResourceCreationSlot<PayloadType> Slot;
		private:
			friend class TRendererResourceSlotCache;
			FEntry* Previous = nullptr;
			FEntry* Next = nullptr;
		};

	private:
		using FIndex = std::unordered_map<KeyType, std::unique_ptr<FEntry>, Hasher>;

	public:
		// Charged node storage, not allocator/driver memory. Includes both key
		// copies, inline payload, list links and conservative hash-node overhead.
		static constexpr size_t AccountedEntryBytes = sizeof(FEntry)
			+ sizeof(typename FIndex::value_type) + 2 * sizeof(void*);
		static constexpr size_t MinimumByteBudget = AccountedEntryBytes + 16 * sizeof(void*);

		explicit TRendererResourceSlotCache(
			ERenderResourceGenerationDependency InPayloadDependencies =
				ERenderResourceGenerationDependency::None)
			: PayloadDependencies(InPayloadDependencies) {}
		TRendererResourceSlotCache(const TRendererResourceSlotCache&) = delete;
		auto operator=(const TRendererResourceSlotCache&) -> TRendererResourceSlotCache& = delete;

		auto FindOrAdd(const KeyType& Key) -> FEntry&
		{
			if (FEntry* Existing = Find(Key)) return *Existing;
			return Add(Key);
		}

		auto FindOrAddBounded(const KeyType& Key, size_t MaximumEntries,
			size_t MaximumBytes = std::numeric_limits<size_t>::max()) -> FEntry&
		{
			check(MaximumEntries > 0 && MaximumBytes >= MinimumByteBudget);
			FEntry* Entry = Find(Key);
			if (!Entry)
			{
				while (Num() >= MaximumEntries) Evict(Oldest);
				Entry = &Add(Key);
			}
			// The current key is newest and remains available to the caller.
			while (Num() > MaximumEntries) Evict(Oldest);
			if (GetAccountedSlotBytes() > MaximumBytes)
			{
				// Release buckets from a formerly larger budget before evicting slots.
				CompactIndex();
				while (Num() > 1 && GetAccountedSlotBytes() > MaximumBytes)
					Evict(Oldest);
				CompactIndex();
				check(GetAccountedSlotBytes() <= MaximumBytes);
			}
			return *Entry;
		}

		auto Find(const KeyType& Key) -> FEntry*
		{
			const auto Found = Index.find(Key);
			if (Found == Index.end()) { ++Misses; return nullptr; }
			++Hits;
			FEntry* Entry = Found->second.get();
			if (Entry != Newest)
			{
				Unlink(Entry);
				Append(Entry);
			}
			return Entry;
		}

		auto Find(const KeyType& Key) const -> const FEntry*
		{
			const auto Found = Index.find(Key);
			return Found != Index.end() ? Found->second.get() : nullptr;
		}

		auto Num() const -> size_t { return Index.size(); }
		auto GetHits() const -> uint64 { return Hits; }
		auto GetMisses() const -> uint64 { return Misses; }
		auto GetEvictions() const -> uint64 { return Evictions; }
		auto GetAccountedSlotBytes() const -> size_t
		{
			return Num() * AccountedEntryBytes + Index.bucket_count() * 2 * sizeof(void*);
		}

		template <typename WeightFunction>
		auto GetRetainedPayloadWeight(WeightFunction&& GetWeight) const -> uint64
		{
			uint64 Total = 0;
			for (const FEntry* Entry = Oldest; Entry; Entry = Entry->Next)
			{
				if (const PayloadType* Payload = Entry->Slot.GetPayload())
				{
					const uint64 Weight = static_cast<uint64>(GetWeight(Entry->Key, *Payload));
					Total = Weight > std::numeric_limits<uint64>::max() - Total
						? std::numeric_limits<uint64>::max() : Total + Weight;
				}
			}
			return Total;
		}

		// "Oldest" means least recently used, rather than earliest insertion.
		auto EvictOldestExcept(const KeyType& RetainedKey) -> bool
		{
			FEntry* Candidate = Oldest;
			if (Candidate && Candidate->Key == RetainedKey) Candidate = Candidate->Next;
			if (!Candidate) return false;
			Evict(Candidate);
			return true;
		}

		auto Reset() -> void
		{
			FIndex{}.swap(Index);
			Oldest = Newest = nullptr;
			NextIndex = 0;
			Hits = Misses = Evictions = 0;
		}

	private:
		auto CompactIndex() -> void
		{
			// rehash(n) need not shrink (MSVC deliberately keeps existing buckets).
			FIndex Compact;
			Compact.reserve(Num());
			while (!Index.empty()) Compact.insert(Index.extract(Index.begin()));
			Index.swap(Compact);
		}

		auto Add(const KeyType& Key) -> FEntry&
		{
			auto Owner = std::make_unique<FEntry>(Key, NextIndex++, PayloadDependencies);
			FEntry* Entry = Owner.get();
			Index.emplace(Key, std::move(Owner));
			Append(Entry);
			return *Entry;
		}
		auto Unlink(FEntry* Entry) -> void
		{
			if (Entry->Previous) Entry->Previous->Next = Entry->Next;
			else Oldest = Entry->Next;
			if (Entry->Next) Entry->Next->Previous = Entry->Previous;
			else Newest = Entry->Previous;
		}
		auto Append(FEntry* Entry) -> void
		{
			Entry->Previous = Newest;
			Entry->Next = nullptr;
			if (Newest) Newest->Next = Entry;
			else Oldest = Entry;
			Newest = Entry;
		}
		auto Evict(FEntry* Entry) -> void
		{
			Unlink(Entry);
			Index.erase(Entry->Key);
			++Evictions;
		}

		ERenderResourceGenerationDependency PayloadDependencies;
		FIndex Index;
		FEntry* Oldest = nullptr;
		FEntry* Newest = nullptr;
		size_t NextIndex = 0;
		uint64 Hits = 0, Misses = 0, Evictions = 0;
	};
}
