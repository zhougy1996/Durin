#pragma once

#include <bitset>

#include "RHIResources.h"
#include "VulkanCompletion.h"

namespace Durin::VulkanRHI
{
	class FVulkanCommandBuffer;
	class FVulkanDevice;
	class FVulkanDynamicRHI;
	class FVulkanGPUTimingManager;

	VULKANRHI_API auto ConvertVulkanTimestampDuration(
		uint64 Begin, uint64 End, uint32 ValidBits, double NanosecondsPerTick,
		bool& bOutOverflow) -> uint64;
	VULKANRHI_API auto PollVulkanGPUTimingForTest(
		FVulkanDynamicRHI& RHI) -> void;

	struct FVulkanGPUTimingStatistics
	{
		uint64 IntervalCapacity = 512;
		uint64 AllocatedPages = 0;
		uint64 LiveIntervals = 0;
		uint64 PendingIntervals = 0;
		uint64 ReadyIntervals = 0;
		uint64 IntervalHighWater = 0;
		uint64 ExhaustionCount = 0;
		uint64 AllocationFailureCount = 0;
		uint64 InvalidRecordingCount = 0;
		uint64 ReuseCount = 0;
		uint64 ResultPollCount = 0;
		uint64 ReadyResultCount = 0;
		uint64 ConversionOverflowCount = 0;
	};
	VULKANRHI_API auto GetVulkanGPUTimingStatisticsForTest(
		FVulkanDynamicRHI& RHI) -> FVulkanGPUTimingStatistics;

	class FVulkanGPUTimingQuery final : public FRHIGPUTimingQuery
	{
	public:
		~FVulkanGPUTimingQuery() override;

	private:
		FVulkanGPUTimingQuery(FVulkanGPUTimingManager& InManager,
			uint32 InPageIndex, uint32 InIntervalIndex, uint32 InGeneration);

		FVulkanGPUTimingManager& Manager;
		uint32 PageIndex = 0;
		uint32 IntervalIndex = 0;
		uint32 Generation = 0;
		FVulkanCompletionToken SubmissionToken = 0;
		bool bCountedReady = false;
		auto SetReady(uint64 DurationNanoseconds) -> void
		{
			PublishReady(DurationNanoseconds);
		}
		auto SetInvalid() -> void { PublishInvalid(); }

		friend class FVulkanGPUTimingManager;
	};

	class FVulkanGPUTimingManager
	{
	public:
		explicit FVulkanGPUTimingManager(FVulkanDevice& InDevice);
		~FVulkanGPUTimingManager();

		auto CreateQuery() -> TRefCountPtr<FVulkanGPUTimingQuery>;
		auto Begin(FVulkanCommandBuffer& CommandBuffer,
			FVulkanGPUTimingQuery& Query) -> void;
		auto End(FVulkanCommandBuffer& CommandBuffer,
			FVulkanGPUTimingQuery& Query) -> void;
		auto MarkSubmitted(FVulkanCompletionToken Token,
			std::span<FVulkanGPUTimingQuery* const> Queries) -> void;
		auto Poll() -> void;
		auto Snapshot() const -> FVulkanGPUTimingStatistics;
		auto ResetStatistics() -> void;

	private:
		static constexpr uint32 IntervalsPerPage = 64;
		static constexpr uint32 MaxPageCount = 8;
		struct FPage
		{
			vk::QueryPool Handle;
			std::bitset<IntervalsPerPage> Used;
			std::array<uint32, IntervalsPerPage> Generations{};
		};

		auto AddPage() -> bool;
		auto ReleaseSlot(uint32 PageIndex, uint32 IntervalIndex,
			uint32 Generation, bool bWasReady) -> void;

		FVulkanDevice& Device;
		std::vector<FPage> Pages;
		std::vector<TRefCountPtr<FVulkanGPUTimingQuery>> PendingQueries;
		uint32 TimestampValidBits = 0;
		double NanosecondsPerTick = 0.0;
		FVulkanGPUTimingStatistics Statistics;

		friend class FVulkanGPUTimingQuery;
	};
}
