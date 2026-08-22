#include "VulkanTransferArena.h"

#include "VulkanBuffer.h"
#include "VulkanDevice.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto AlignUp(uint64 Value, uint64 Alignment) -> uint64
		{
			check(Alignment > 0);
			const uint64 Remainder = Value % Alignment;
			if (Remainder == 0)
			{
				return Value;
			}
			const uint64 Padding = Alignment - Remainder;
			check(Value <= std::numeric_limits<uint64>::max() - Padding);
			return Value + Padding;
		}
	}

	FVulkanTransferRange::FVulkanTransferRange(
		FVulkanTransferRange&& Other) noexcept
	{
		*this = std::move(Other);
	}

	auto FVulkanTransferRange::operator=(
		FVulkanTransferRange&& Other) noexcept -> FVulkanTransferRange&
	{
		if (this == &Other)
		{
			return *this;
		}
		if (Owner)
		{
			Owner->Cancel(*this);
		}
		Owner = std::exchange(Other.Owner, nullptr);
		Page = std::exchange(Other.Page, nullptr);
		Buffer = std::exchange(Other.Buffer, nullptr);
		Offset = std::exchange(Other.Offset, 0);
		Size = std::exchange(Other.Size, 0);
		Token = std::exchange(Other.Token, 0);
		bOversize = std::exchange(Other.bOversize, false);
		return *this;
	}

	FVulkanTransferRange::~FVulkanTransferRange()
	{
		if (Owner)
		{
			Owner->Cancel(*this);
		}
	}

	auto FVulkanTransferRange::GetMappedPointer() const -> std::byte*
	{
		check(Buffer);
		return static_cast<std::byte*>(Buffer->GetMappedPointer()) + Offset;
	}

	auto FVulkanTransferRange::Flush() const -> void
	{
		check(Buffer && Offset <= std::numeric_limits<uint32>::max()
			&& Size <= std::numeric_limits<uint32>::max());
		Buffer->FlushMappedMemory(static_cast<uint32>(Offset),
			static_cast<uint32>(Size));
	}

	auto FVulkanTransferRange::Invalidate() const -> void
	{
		check(Buffer && Offset <= std::numeric_limits<uint32>::max()
			&& Size <= std::numeric_limits<uint32>::max());
		Buffer->InvalidateMappedMemory(static_cast<uint32>(Offset),
			static_cast<uint32>(Size));
	}

	auto FVulkanTransferRange::Retire() -> void
	{
		check(Owner);
		Owner->Retire(*this);
	}

	FVulkanTransferArena::FVulkanTransferArena(FVulkanDevice& InDevice,
		FVulkanTransferArenaConfig InConfig)
		: Device(InDevice)
		, Config(InConfig)
	{
		check(Config.PageSize > 0 && Config.MaxPageCount > 0
			&& Config.PageSize <= std::numeric_limits<uint32>::max());
		check(Config.AllocationClass
			== EVulkanAllocationClassCandidate::TransferUpload
			|| Config.AllocationClass
				== EVulkanAllocationClassCandidate::TransferReadback);
	}

	FVulkanTransferArena::~FVulkanTransferArena()
	{
		CheckVulkanRHIThread();
		Device.GetCompletionTracker().WaitForAll();
		ReclaimCompleted();
		for (auto& Page : OversizePages)
		{
			DestroyPage(*Page);
		}
		OversizePages.clear();
		for (auto& Page : Pages)
		{
			DestroyPage(*Page);
		}
		Pages.clear();
	}

	auto FVulkanTransferArena::Acquire(uint64 Size, uint64 Alignment,
		FVulkanCompletionToken Token) -> FVulkanTransferAcquireResult
	{
		CheckVulkanRHIThread();
		check(Size > 0 && Size <= std::numeric_limits<uint32>::max()
			&& Alignment > 0 && Token > 0);
		ReclaimCompleted();
		if (Size > Config.PageSize)
		{
			try
			{
				FPage* Page = CreatePage(Size, true);
				auto Range = TryAllocateFromPage(*Page, Size, Alignment, Token);
				check(Range);
				return {.Range = std::move(Range)};
			}
			catch (const std::exception& Error)
			{
				DURIN_ERROR("Vulkan transfer oversize allocation failed: class={}, bytes={}, error={}.",
					static_cast<uint32>(Config.AllocationClass), Size, Error.what());
				return {.bAllocationFailed = true};
			}
		}

		for (const auto& Page : Pages)
		{
			if (auto Range = TryAllocateFromPage(*Page, Size, Alignment, Token))
			{
				return {.Range = std::move(Range)};
			}
		}
		if (Pages.size() < Config.MaxPageCount)
		{
			try
			{
				FPage* Page = CreatePage(Config.PageSize, false);
				auto Range = TryAllocateFromPage(*Page, Size, Alignment, Token);
				check(Range);
				return {.Range = std::move(Range)};
			}
			catch (const std::exception& Error)
			{
				DURIN_ERROR("Vulkan transfer page allocation failed: class={}, bytes={}, error={}.",
					static_cast<uint32>(Config.AllocationClass),
					Config.PageSize, Error.what());
				return {.bAllocationFailed = true};
			}
		}

		GVulkanMemoryBaselineTracker.RecordArenaOverflow(Config.AllocationClass);
		return {.WaitToken = GetOldestRetiredToken()};
	}

	auto FVulkanTransferArena::ReclaimCompleted() -> void
	{
		CheckVulkanRHIThread();
		Device.GetCompletionTracker().Poll();
		const FVulkanCompletionToken Completed =
			Device.GetCompletionTracker().GetCompletedToken();
		for (auto& Page : Pages)
		{
			std::erase_if(Page->RetiredRanges,
				[this, &Page, Completed](const FRetiredRange& Range) {
					if (Range.Token > Completed)
					{
						return false;
					}
					InsertFreeRange(*Page, {Range.Offset, Range.Size});
					GVulkanMemoryBaselineTracker.RecordArenaRangeReclaimed(
						Config.AllocationClass, Range.Size);
					return true;
				});
		}
		std::erase_if(OversizePages,
			[this, Completed](const std::unique_ptr<FPage>& Page) {
				if (Page->RetiredRanges.empty()
					|| Page->RetiredRanges.front().Token > Completed)
				{
					return false;
				}
				GVulkanMemoryBaselineTracker.RecordArenaRangeReclaimed(
					Config.AllocationClass, Page->RetiredRanges.front().Size);
				DestroyPage(*Page);
				return true;
			});
	}

	auto FVulkanTransferArena::GetPageCount() const -> uint32
	{
		return static_cast<uint32>(Pages.size());
	}

	auto FVulkanTransferArena::CreatePage(uint64 Size, bool bOversize) -> FPage*
	{
		check(Size <= std::numeric_limits<uint32>::max());
		EBufferUsageFlags Usage = EBufferUsageFlags::Dynamic;
		if (Config.AllocationClass
			== EVulkanAllocationClassCandidate::TransferUpload)
		{
			Usage |= EBufferUsageFlags::SourceCopy;
		}
		else
		{
			Usage |= EBufferUsageFlags::DestinationCopy
				| EBufferUsageFlags::KeepCPUAccessible;
		}
		auto Page = std::make_unique<FPage>();
		Page->Size = Size;
		Page->bOversize = bOversize;
		Page->Buffer = new FVulkanBuffer(Device, FRHIBufferCreateDesc::Create(
			Config.DebugName, static_cast<uint32>(Size), 0, Usage));
		Page->FreeRanges.push_back({0, Size});
		FPage* Result = Page.get();
		if (bOversize)
		{
			OversizePages.push_back(std::move(Page));
		}
		else
		{
			Pages.push_back(std::move(Page));
			GVulkanMemoryBaselineTracker.RecordArenaPageAllocated(
				Config.AllocationClass, Size);
		}
		return Result;
	}

	auto FVulkanTransferArena::TryAllocateFromPage(FPage& Page, uint64 Size,
		uint64 Alignment, FVulkanCompletionToken Token) -> FVulkanTransferRange
	{
		for (auto It = Page.FreeRanges.begin(); It != Page.FreeRanges.end(); ++It)
		{
			const uint64 AlignedOffset = AlignUp(It->Offset, Alignment);
			if (AlignedOffset < It->Offset || AlignedOffset - It->Offset > It->Size
				|| Size > It->Size - (AlignedOffset - It->Offset))
			{
				continue;
			}
			const FFreeRange Original = *It;
			It = Page.FreeRanges.erase(It);
			if (AlignedOffset > Original.Offset)
			{
				It = Page.FreeRanges.insert(It,
					{Original.Offset, AlignedOffset - Original.Offset});
				++It;
			}
			const uint64 AllocatedEnd = AlignedOffset + Size;
			const uint64 OriginalEnd = Original.Offset + Original.Size;
			if (AllocatedEnd < OriginalEnd)
			{
				Page.FreeRanges.insert(It,
					{AllocatedEnd, OriginalEnd - AllocatedEnd});
			}
			const bool bReuse = Page.bHasServedAllocation;
			Page.bHasServedAllocation = true;
			GVulkanMemoryBaselineTracker.RecordArenaRangeAllocated(
				Config.AllocationClass, Size, bReuse, Page.bOversize);
			FVulkanTransferRange Result;
			Result.Owner = this;
			Result.Page = &Page;
			Result.Buffer = Page.Buffer.GetReference();
			Result.Offset = AlignedOffset;
			Result.Size = Size;
			Result.Token = Token;
			Result.bOversize = Page.bOversize;
			return Result;
		}
		return {};
	}

	auto FVulkanTransferArena::GetOldestRetiredToken() const
		-> FVulkanCompletionToken
	{
		FVulkanCompletionToken Oldest = 0;
		auto Consider = [&Oldest](const FPage& Page) {
			for (const FRetiredRange& Range : Page.RetiredRanges)
			{
				if (Oldest == 0 || Range.Token < Oldest)
				{
					Oldest = Range.Token;
				}
			}
		};
		for (const auto& Page : Pages) Consider(*Page);
		for (const auto& Page : OversizePages) Consider(*Page);
		return Oldest;
	}

	auto FVulkanTransferArena::Cancel(FVulkanTransferRange& Range) -> void
	{
		check(Range.Owner == this && Range.Page);
		auto& Page = *static_cast<FPage*>(Range.Page);
		GVulkanMemoryBaselineTracker.RecordArenaRangeReclaimed(
			Config.AllocationClass, Range.Size);
		if (Range.bOversize)
		{
			const FPage* PageAddress = &Page;
			std::erase_if(OversizePages,
				[this, PageAddress](const std::unique_ptr<FPage>& Candidate) {
					if (Candidate.get() != PageAddress) return false;
					DestroyPage(*Candidate);
					return true;
				});
		}
		else
		{
			InsertFreeRange(Page, {Range.Offset, Range.Size});
		}
		Range.Owner = nullptr;
		Range.Page = nullptr;
		Range.Buffer = nullptr;
	}

	auto FVulkanTransferArena::Retire(FVulkanTransferRange& Range) -> void
	{
		check(Range.Owner == this && Range.Page && Range.Token > 0);
		auto& Page = *static_cast<FPage*>(Range.Page);
		Page.RetiredRanges.push_back({Range.Offset, Range.Size, Range.Token});
		Range.Owner = nullptr;
		Range.Page = nullptr;
		Range.Buffer = nullptr;
	}

	auto FVulkanTransferArena::InsertFreeRange(FPage& Page, FFreeRange Range)
		-> void
	{
		Page.FreeRanges.push_back(Range);
		std::ranges::sort(Page.FreeRanges, {}, &FFreeRange::Offset);
		std::vector<FFreeRange> Coalesced;
		for (const FFreeRange& Candidate : Page.FreeRanges)
		{
			if (!Coalesced.empty()
				&& Coalesced.back().Offset + Coalesced.back().Size
					== Candidate.Offset)
			{
				Coalesced.back().Size += Candidate.Size;
			}
			else
			{
				Coalesced.push_back(Candidate);
			}
		}
		Page.FreeRanges = std::move(Coalesced);
	}

	auto FVulkanTransferArena::DestroyPage(FPage& Page) -> void
	{
		if (!Page.bOversize)
		{
			GVulkanMemoryBaselineTracker.RecordArenaPageFreed(
				Config.AllocationClass, Page.Size);
		}
		Page.Buffer = nullptr;
	}
}
