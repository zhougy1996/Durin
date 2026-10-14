#pragma once

#include "CoreMinimal.h"
#include "Backend/RHIDynamicBufferWrite.h"

namespace Durin
{
	// Shared mapped pages with atom-aligned slices. Native allocation is supplied
	// externally; acquisition only observes published GPU metadata and never waits.
	class FRHIDynamicBufferPagePool final
	{
	public:
		FRHIDynamicBufferPagePool(uint32 InMaxPages, uint64 InMaxBytes)
			: MaxPages(InMaxPages), MaxBytes(InMaxBytes) {}
		auto AddPage(std::shared_ptr<void> Owner, void* Data, uint32 Size) -> bool
		{
			require(Owner && Data && Size);
			std::lock_guard Lock(Mutex);
			if (Pages.size() >= MaxPages || Size > MaxBytes - ReservedBytes) return false;
			Pages.push_back({std::move(Owner), static_cast<std::byte*>(Data), {{0, Size}}, {}});
			ReservedBytes += Size;
			return true;
		}
		auto CanAddPage(uint32 Size) -> bool
		{
			std::lock_guard Lock(Mutex);
			return Size && Pages.size() < MaxPages && Size <= MaxBytes - ReservedBytes;
		}
		auto GetPageCount() -> size_t { std::lock_guard Lock(Mutex); return Pages.size(); }
		auto TryAcquire(uint32 Size, uint32 Alignment,
			FRHIDynamicBufferWritePoolSnapshot* Snapshot = nullptr)
			-> std::shared_ptr<FRHIDynamicBufferWriteRange>
		{
			require(Size && Alignment);
			std::lock_guard Lock(Mutex);
			if (Snapshot)
			{
				*Snapshot = {};
				Snapshot->PageCount = static_cast<uint32>(Pages.size());
				Snapshot->ReservedBytes = ReservedBytes;
			}
			ReclaimPages();
			// Diagnostics count live slices, not hypothetical slots or free byte spans.
			if (Snapshot)
				for (const auto& Page : Pages)
				{
					for (const auto& Span : Page.Free) Snapshot->FreeBytes += Span.Size;
					for (const auto& Allocation : Page.Allocations)
					{
						++Snapshot->RangeCount;
						if (Allocation.Range.use_count() != 1) ++Snapshot->RetainedRangeCount;
						else ++Snapshot->AwaitingRetirementRangeCount;
					}
				}
			const auto Placement = FindFreeSpan(Size, Alignment);
			if (!Placement) return {};
			auto& Page = *Placement->Page;
			const size_t Index = Placement->Index;
			const auto Span = Page.Free[Index];
			const uint64 Start = Placement->Start, End = Start + Placement->PaddedSize;
			const uint64 SpanEnd = uint64(Span.Offset) + Span.Size;
			auto Range = std::make_shared<FRHIDynamicBufferWriteRange>(Page.Owner, Page.Data + Start);
			Range->Offset = static_cast<uint32>(Start);
			Range->Size = Size;
			// Publish allocation ownership before modifying the free list.
			Page.Allocations.push_back({Range->Offset, Placement->PaddedSize, Range});
			if (Start == Span.Offset && End == SpanEnd)
				Page.Free.erase(Page.Free.begin() + Index);
			else
			{
				// Keep the previous erase-and-append first-fit order: other spans,
				// then the prefix and suffix. Update the retained slot in place.
				std::move(Page.Free.begin() + Index + 1, Page.Free.end(), Page.Free.begin() + Index);
				if (Start > Span.Offset)
				{
					Page.Free.back() = {Span.Offset, static_cast<uint32>(Start - Span.Offset)};
					if (End < SpanEnd) Page.Free.push_back({static_cast<uint32>(End), static_cast<uint32>(SpanEnd - End)});
				}
				else Page.Free.back() = {static_cast<uint32>(End), static_cast<uint32>(SpanEnd - End)};
			}
			return Range;
		}
		// Reclaims completed slices but neither reserves a range nor changes the
		// remaining free spans. Used to decide whether native growth is necessary.
		auto CanAcquire(uint32 Size, uint32 Alignment) -> bool
		{
			require(Size && Alignment);
			std::lock_guard Lock(Mutex);
			ReclaimPages();
			return FindFreeSpan(Size, Alignment).has_value();
		}
	private:
		struct FSpan { uint32 Offset, Size; };
		struct FAllocation { uint32 Offset, Size; std::shared_ptr<FRHIDynamicBufferWriteRange> Range; };
		struct FPage
		{
			std::shared_ptr<void> Owner;
			std::byte* Data;
			std::vector<FSpan> Free;
			std::vector<FAllocation> Allocations;
		};
		struct FPlacement { FPage* Page; size_t Index; uint32 Start, PaddedSize; };
		// Caller holds Mutex. Probe and acquisition use exactly the same fit rules.
		auto FindFreeSpan(uint32 Size, uint32 Alignment) -> std::optional<FPlacement>
		{
			const uint64 PaddedSize = (uint64(Size) + Alignment - 1) / Alignment * Alignment;
			for (auto& Page : Pages)
				for (size_t Index = 0; Index < Page.Free.size(); ++Index)
				{
					const auto Span = Page.Free[Index];
					const uint64 Start = (uint64(Span.Offset) + Alignment - 1) / Alignment * Alignment;
					if (Start + PaddedSize <= uint64(Span.Offset) + Span.Size)
						return FPlacement{&Page, Index, static_cast<uint32>(Start), static_cast<uint32>(PaddedSize)};
				}
			return {};
		}
		auto ReclaimPages() -> void
		{
			for (auto& Page : Pages)
			{
				const auto Reclaimed = std::erase_if(Page.Allocations, [&](const FAllocation& Allocation) {
					if (Allocation.Range.use_count() != 1) return false;
					bool bReusable;
					{
						std::lock_guard RangeLock(Allocation.Range->Mutex);
						bReusable = Allocation.Range.use_count() == 1 && Allocation.Range->Uses.IsRetirementEligible();
					}
					if (!bReusable) return false;
					Page.Free.push_back({Allocation.Offset, Allocation.Size});
					return true;
				});
				if (!Reclaimed) continue;
				std::ranges::sort(Page.Free, {}, &FSpan::Offset);
				size_t Count = 0;
				for (const auto Span : Page.Free)
				{
					if (Count && uint64(Page.Free[Count - 1].Offset) + Page.Free[Count - 1].Size == Span.Offset)
						Page.Free[Count - 1].Size += Span.Size;
					else Page.Free[Count++] = Span;
				}
				Page.Free.resize(Count);
			}
		}
		std::mutex Mutex;
		uint32 MaxPages;
		uint64 MaxBytes, ReservedBytes = 0;
		std::vector<FPage> Pages;
	};
}
