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
			const uint64 PaddedSize = (uint64(Size) + Alignment - 1) / Alignment * Alignment;
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
				for (size_t Index = 1; Index < Page.Free.size();)
				{
					auto& Previous = Page.Free[Index - 1];
					if (uint64(Previous.Offset) + Previous.Size == Page.Free[Index].Offset)
					{
						Previous.Size += Page.Free[Index].Size;
						Page.Free.erase(Page.Free.begin() + Index);
					}
					else ++Index;
				}
			}
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
			for (auto& Page : Pages)
				for (size_t Index = 0; Index < Page.Free.size(); ++Index)
				{
					const auto Span = Page.Free[Index];
					const uint64 Start = (uint64(Span.Offset) + Alignment - 1) / Alignment * Alignment;
					const uint64 End = Start + PaddedSize;
					const uint64 SpanEnd = uint64(Span.Offset) + Span.Size;
					if (End > SpanEnd) continue;
					auto Range = std::make_shared<FRHIDynamicBufferWriteRange>(Page.Owner, Page.Data + Start);
					Range->Offset = static_cast<uint32>(Start);
					Range->Size = Size;
					// Publish allocation ownership before modifying the free list.
					Page.Allocations.push_back({Range->Offset, static_cast<uint32>(PaddedSize), Range});
					Page.Free.erase(Page.Free.begin() + Index);
					if (Start > Span.Offset) Page.Free.push_back({Span.Offset, static_cast<uint32>(Start - Span.Offset)});
					if (End < SpanEnd) Page.Free.push_back({static_cast<uint32>(End), static_cast<uint32>(SpanEnd - End)});
					return Range;
				}
			return {};
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
		std::mutex Mutex;
		uint32 MaxPages;
		uint64 MaxBytes, ReservedBytes = 0;
		std::vector<FPage> Pages;
	};
}
