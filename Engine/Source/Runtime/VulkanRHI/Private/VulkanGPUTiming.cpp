#include "VulkanGPUTiming.h"

#include "RHICommandList.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"
#include "VulkanQueue.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	auto PollVulkanGPUTimingForTest(FVulkanDynamicRHI& RHI) -> void
	{
		auto& Device = *RHI.GetDeviceForTesting();
		Device.GetCompletionTracker().Poll();
		Device.GetGPUTimingManager().Poll();
	}

	auto GetVulkanGPUTimingStatisticsForTest(FVulkanDynamicRHI& RHI)
		-> FVulkanGPUTimingStatistics
	{
		if (IsInRHIThread())
			return RHI.GetDeviceForTesting()->GetGPUTimingManager().Snapshot();
		FVulkanGPUTimingStatistics Result;
		GCommandListExecutor.ExecuteSynchronousOperation(false, [&]() {
			Result = RHI.GetDeviceForTesting()->GetGPUTimingManager().Snapshot();
		});
		return Result;
	}

	auto ConvertVulkanTimestampDuration(uint64 Begin, uint64 End,
		uint32 ValidBits, double NanosecondsPerTick, bool& bOutOverflow) -> uint64
	{
		bOutOverflow = false;
		if (ValidBits == 0 || ValidBits > 64
			|| !std::isfinite(NanosecondsPerTick) || NanosecondsPerTick <= 0.0)
			return 0;
		const uint64 Mask = ValidBits == 64
			? std::numeric_limits<uint64>::max() : (uint64{1} << ValidBits) - 1;
		const uint64 Ticks = ((End & Mask) - (Begin & Mask)) & Mask;
		const long double Nanoseconds = static_cast<long double>(Ticks)
			* static_cast<long double>(NanosecondsPerTick);
		if (Nanoseconds >= static_cast<long double>(
			std::numeric_limits<uint64>::max()))
		{
			bOutOverflow = true;
			return std::numeric_limits<uint64>::max();
		}
		return static_cast<uint64>(std::llround(Nanoseconds));
	}

	FVulkanGPUTimingQuery::FVulkanGPUTimingQuery(
		FVulkanGPUTimingManager& InManager, uint32 InPageIndex,
		uint32 InIntervalIndex, uint32 InGeneration)
		: Manager(InManager), PageIndex(InPageIndex),
		  IntervalIndex(InIntervalIndex), Generation(InGeneration)
	{
	}

	FVulkanGPUTimingQuery::~FVulkanGPUTimingQuery()
	{
		Manager.ReleaseSlot(PageIndex, IntervalIndex, Generation, bCountedReady);
	}

	FVulkanGPUTimingManager::FVulkanGPUTimingManager(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
		const uint32 Family = Device.GetGraphicsQueue()->GetFamilyIndex();
		TimestampValidBits =
			Device.GetQueueFamilyProperties(Family).timestampValidBits;
		NanosecondsPerTick = Device.GetGpuProperties().limits.timestampPeriod;
	}

	FVulkanGPUTimingManager::~FVulkanGPUTimingManager()
	{
		check(PendingQueries.empty());
		for (const FPage& Page : Pages)
		{
			check(Page.Used.none());
			Device.GetHandle().destroyQueryPool(Page.Handle);
		}
	}

	auto FVulkanGPUTimingManager::Snapshot() const
		-> FVulkanGPUTimingStatistics
	{
		FVulkanGPUTimingStatistics Result = Statistics;
		Result.InvalidRecordingCount =
			FRHIGPUTimingQuery::GetInvalidRecordingCount();
		return Result;
	}

	auto FVulkanGPUTimingManager::ResetStatistics() -> void
	{
		const uint64 PagesValue = Statistics.AllocatedPages;
		const uint64 Live = Statistics.LiveIntervals;
		const uint64 Pending = Statistics.PendingIntervals;
		const uint64 Ready = Statistics.ReadyIntervals;
		Statistics = {};
		Statistics.IntervalCapacity = MaxPageCount * IntervalsPerPage;
		Statistics.AllocatedPages = PagesValue;
		Statistics.LiveIntervals = Live;
		Statistics.PendingIntervals = Pending;
		Statistics.ReadyIntervals = Ready;
		Statistics.IntervalHighWater = Live;
		FRHIGPUTimingQuery::ResetInvalidRecordingCount();
	}

	auto FVulkanGPUTimingManager::AddPage() -> bool
	{
		if (Pages.size() >= MaxPageCount) return false;
		vk::QueryPoolCreateInfo Info;
		Info.setQueryType(vk::QueryType::eTimestamp)
			.setQueryCount(IntervalsPerPage * 2);
		FPage Page;
		try
		{
#if DURIN_VULKAN_TEST_FAILURE_INJECTION
			ThrowIfVulkanNativeCreateFailureIsArmed(
				EVulkanCreateFailurePoint::QueryPool);
#endif
			Page.Handle = Device.GetHandle().createQueryPool(Info);
		}
		catch (...) { ++Statistics.AllocationFailureCount; return false; }
		Device.GetRHI().GetDebugUtils().NameObject(Page.Handle,
			std::format("Durin.TimestampQueryPool.{}", Pages.size()));
		Pages.push_back(std::move(Page));
		Statistics.AllocatedPages = Pages.size();
		return true;
	}

	auto FVulkanGPUTimingManager::CreateQuery()
		-> TRefCountPtr<FVulkanGPUTimingQuery>
	{
		CheckVulkanRHIThread();
		for (;;)
		{
			for (uint32 PageIndex = 0; PageIndex < Pages.size(); ++PageIndex)
			{
				FPage& Page = Pages[PageIndex];
				for (uint32 Interval = 0; Interval < IntervalsPerPage; ++Interval)
				{
					if (Page.Used.test(Interval)) continue;
					Page.Used.set(Interval);
					const uint32 Generation = ++Page.Generations[Interval];
					++Statistics.LiveIntervals;
					Statistics.IntervalHighWater = std::max(
						Statistics.IntervalHighWater, Statistics.LiveIntervals);
					if (Generation > 1) ++Statistics.ReuseCount;
					return new FVulkanGPUTimingQuery(
						*this, PageIndex, Interval, Generation);
				}
			}
			if (Pages.size() >= MaxPageCount)
			{
				++Statistics.ExhaustionCount;
				return nullptr;
			}
			if (!AddPage()) return nullptr;
		}
	}

	auto FVulkanGPUTimingManager::Begin(FVulkanCommandBuffer& CommandBuffer,
		FVulkanGPUTimingQuery& Query) -> void
	{
		if (Query.bCountedReady)
		{
			Query.bCountedReady = false;
			check(Statistics.ReadyIntervals != 0);
			--Statistics.ReadyIntervals;
		}
		FPage& Page = Pages[Query.PageIndex];
		const uint32 First = Query.IntervalIndex * 2;
		CommandBuffer.GetHandle().resetQueryPool(Page.Handle, First, 2);
		CommandBuffer.GetHandle().writeTimestamp(
			vk::PipelineStageFlagBits::eTopOfPipe, Page.Handle, First);
	}

	auto FVulkanGPUTimingManager::End(FVulkanCommandBuffer& CommandBuffer,
		FVulkanGPUTimingQuery& Query) -> void
	{
		FPage& Page = Pages[Query.PageIndex];
		CommandBuffer.GetHandle().writeTimestamp(
			vk::PipelineStageFlagBits::eBottomOfPipe, Page.Handle,
			Query.IntervalIndex * 2 + 1);
		check(Query.CommitRecording());
	}

	auto FVulkanGPUTimingManager::MarkSubmitted(FVulkanCompletionToken Token,
		std::span<FVulkanGPUTimingQuery* const> Queries) -> void
	{
		for (FVulkanGPUTimingQuery* Query : Queries)
		{
			Query->SubmissionToken = Token;
			PendingQueries.emplace_back(Query);
			++Statistics.PendingIntervals;
		}
	}

	auto FVulkanGPUTimingManager::Poll() -> void
	{
		const FVulkanCompletionToken Completed =
			Device.GetCompletionTracker().GetCompletedToken();
		auto NewEnd = std::remove_if(PendingQueries.begin(), PendingQueries.end(),
			[&](const TRefCountPtr<FVulkanGPUTimingQuery>& Query) {
				if (Query->SubmissionToken > Completed) return false;
				++Statistics.ResultPollCount;
				const FPage& Page = Pages[Query->PageIndex];
				std::array<uint64, 2> Values{};
				const VkResult Result = vkGetQueryPoolResults(
					static_cast<VkDevice>(Device.GetHandle()),
					static_cast<VkQueryPool>(Page.Handle), Query->IntervalIndex * 2, 2,
					sizeof(Values), Values.data(), sizeof(uint64), VK_QUERY_RESULT_64_BIT);
				if (Result == VK_NOT_READY) return false;
				if (Result != VK_SUCCESS)
				{
					Query->SetInvalid();
					--Statistics.PendingIntervals;
					return true;
				}
				bool bOverflow = false;
				Query->SetReady(ConvertVulkanTimestampDuration(
					Values[0], Values[1], TimestampValidBits,
					NanosecondsPerTick, bOverflow));
				--Statistics.PendingIntervals;
				++Statistics.ReadyIntervals;
				++Statistics.ReadyResultCount;
				if (bOverflow) ++Statistics.ConversionOverflowCount;
				Query->bCountedReady = true;
				return true;
			});
		PendingQueries.erase(NewEnd, PendingQueries.end());
	}

	auto FVulkanGPUTimingManager::ReleaseSlot(uint32 PageIndex,
		uint32 IntervalIndex, uint32 Generation, bool bWasReady) -> void
	{
		CheckVulkanRHIThread();
		check(PageIndex < Pages.size());
		FPage& Page = Pages[PageIndex];
		check(Page.Generations[IntervalIndex] == Generation);
		check(Page.Used.test(IntervalIndex));
		Page.Used.reset(IntervalIndex);
		check(Statistics.LiveIntervals != 0);
		--Statistics.LiveIntervals;
		if (bWasReady)
		{
			check(Statistics.ReadyIntervals != 0);
			--Statistics.ReadyIntervals;
		}
	}
}
