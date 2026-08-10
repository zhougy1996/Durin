#include "VulkanDiagnostics.h"

namespace Durin::VulkanRHI
{
	FVulkanMemoryBaselineTracker GVulkanMemoryBaselineTracker;

	namespace
	{
		auto GetAllocationSizeBucket(uint64 Size) -> uint32
		{
			uint32 Bucket = 0;
			uint64 UpperBound = 4096;
			while (Size > UpperBound
				&& Bucket + 1 < FVulkanAllocationCandidateStatistics::SizeBucketCount)
			{
				++Bucket;
				UpperBound = FVulkanMemoryBaselineTracker::SaturatingAdd(
					UpperBound, UpperBound);
			}
			return Bucket;
		}
	}

	auto FVulkanMemoryBaselineTracker::SaturatingAdd(
		uint64 Left, uint64 Right) -> uint64
	{
		const uint64 Maximum = std::numeric_limits<uint64>::max();
		return Right > Maximum - Left ? Maximum : Left + Right;
	}

	auto FVulkanMemoryBaselineTracker::BeginFrame() -> void
	{
		std::lock_guard Lock(Mutex);
		CurrentFrameUploadBytes = 0;
		CurrentFrameReadbackBytes = 0;
	}

	auto FVulkanMemoryBaselineTracker::RecordAllocation(
		EVulkanAllocationClassCandidate Candidate,
		uint64 RequestedBytes,
		uint64 ActualBytes,
		bool bDedicated) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Class = Statistics.AllocationClasses[static_cast<uint32>(Candidate)];
		Class.AllocationCount = SaturatingAdd(Class.AllocationCount, 1);
		Class.RequestedBytes = SaturatingAdd(Class.RequestedBytes, RequestedBytes);
		Class.ActualBytes = SaturatingAdd(Class.ActualBytes, ActualBytes);
		Class.LiveAllocationCount = SaturatingAdd(Class.LiveAllocationCount, 1);
		Class.LiveBytes = SaturatingAdd(Class.LiveBytes, ActualBytes);
		Class.PeakLiveBytes = std::max(Class.PeakLiveBytes, Class.LiveBytes);
		if (bDedicated)
		{
			Class.DedicatedAllocationCount = SaturatingAdd(
				Class.DedicatedAllocationCount, 1);
		}
		Class.PeakAllocationBytes = std::max(Class.PeakAllocationBytes, ActualBytes);
		auto& Bucket = Class.SizeBuckets[GetAllocationSizeBucket(RequestedBytes)];
		Bucket = SaturatingAdd(Bucket, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordAllocationFailure(
		EVulkanAllocationClassCandidate Candidate) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Count = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.AllocationFailureCount;
		Count = SaturatingAdd(Count, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordAllocationFreed(
		EVulkanAllocationClassCandidate Candidate, uint64 ActualBytes) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Class = Statistics.AllocationClasses[static_cast<uint32>(Candidate)];
		Class.LiveAllocationCount = Class.LiveAllocationCount == 0
			? 0 : Class.LiveAllocationCount - 1;
		Class.LiveBytes = ActualBytes >= Class.LiveBytes
			? 0 : Class.LiveBytes - ActualBytes;
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaPageAllocated(
		EVulkanAllocationClassCandidate Candidate, uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaCapacityBytes;
		Value = SaturatingAdd(Value, Bytes);
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaPageFreed(
		EVulkanAllocationClassCandidate Candidate, uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaCapacityBytes;
		Value = Bytes >= Value ? 0 : Value - Bytes;
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaRangeAllocated(
		EVulkanAllocationClassCandidate Candidate, uint64 Bytes,
		bool bReuse, bool bOversize) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Class = Statistics.AllocationClasses[static_cast<uint32>(Candidate)];
		Class.ArenaLiveBytes = SaturatingAdd(Class.ArenaLiveBytes, Bytes);
		Class.ArenaHighWaterBytes = std::max(
			Class.ArenaHighWaterBytes, Class.ArenaLiveBytes);
		if (bReuse)
		{
			Class.ArenaReuseCount = SaturatingAdd(Class.ArenaReuseCount, 1);
		}
		if (bOversize)
		{
			Class.ArenaOversizeCount = SaturatingAdd(Class.ArenaOversizeCount, 1);
		}
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaRangeReclaimed(
		EVulkanAllocationClassCandidate Candidate, uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaLiveBytes;
		Value = Bytes >= Value ? 0 : Value - Bytes;
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaOverflow(
		EVulkanAllocationClassCandidate Candidate) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaOverflowCount;
		Value = SaturatingAdd(Value, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordArenaWait(
		EVulkanAllocationClassCandidate Candidate) -> void
	{
		std::lock_guard Lock(Mutex);
		auto& Value = Statistics.AllocationClasses[static_cast<uint32>(Candidate)]
			.ArenaWaitCount;
		Value = SaturatingAdd(Value, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordUpload(uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.UploadOperationCount = SaturatingAdd(
			Statistics.UploadOperationCount, 1);
		Statistics.UploadBytes = SaturatingAdd(Statistics.UploadBytes, Bytes);
		CurrentFrameUploadBytes = SaturatingAdd(CurrentFrameUploadBytes, Bytes);
		Statistics.PeakUploadBytesPerFrame = std::max(
			Statistics.PeakUploadBytesPerFrame, CurrentFrameUploadBytes);
	}

	auto FVulkanMemoryBaselineTracker::RecordReadback(uint64 Bytes) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.ReadbackOperationCount = SaturatingAdd(
			Statistics.ReadbackOperationCount, 1);
		Statistics.ReadbackBytes = SaturatingAdd(Statistics.ReadbackBytes, Bytes);
		CurrentFrameReadbackBytes = SaturatingAdd(CurrentFrameReadbackBytes, Bytes);
		Statistics.PeakReadbackBytesPerFrame = std::max(
			Statistics.PeakReadbackBytesPerFrame, CurrentFrameReadbackBytes);
	}

	auto FVulkanMemoryBaselineTracker::RecordFrameFenceWait(uint64 Nanoseconds) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.FrameFenceWaitCount = SaturatingAdd(
			Statistics.FrameFenceWaitCount, 1);
		Statistics.FrameFenceWaitNanoseconds = SaturatingAdd(
			Statistics.FrameFenceWaitNanoseconds, Nanoseconds);
	}

	auto FVulkanMemoryBaselineTracker::RecordCommandBufferAllocation() -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.CommandBufferAllocationCount = SaturatingAdd(
			Statistics.CommandBufferAllocationCount, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordCommandBufferReuse() -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.CommandBufferReuseCount = SaturatingAdd(
			Statistics.CommandBufferReuseCount, 1);
	}

	auto FVulkanMemoryBaselineTracker::RecordDescriptorPoolCreated(
		uint64 SetCapacity) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DescriptorPoolCount = SaturatingAdd(
			Statistics.DescriptorPoolCount, 1);
		Statistics.DescriptorPoolSetCapacity = SaturatingAdd(
			Statistics.DescriptorPoolSetCapacity, SetCapacity);
	}

	auto FVulkanMemoryBaselineTracker::RecordDescriptorSetsAllocated(
		uint64 SetCount) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DescriptorAllocatedSetCount = SaturatingAdd(
			Statistics.DescriptorAllocatedSetCount, SetCount);
		Statistics.DescriptorPeakAllocatedSetCount = std::max(
			Statistics.DescriptorPeakAllocatedSetCount,
			Statistics.DescriptorAllocatedSetCount);
	}

	auto FVulkanMemoryBaselineTracker::RecordDescriptorPoolDestroyed(
		uint64 SetCapacity, uint64 AllocatedSetCount) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DescriptorPoolCount =
			Statistics.DescriptorPoolCount == 0
			? 0 : Statistics.DescriptorPoolCount - 1;
		Statistics.DescriptorPoolSetCapacity =
			SetCapacity >= Statistics.DescriptorPoolSetCapacity
			? 0 : Statistics.DescriptorPoolSetCapacity - SetCapacity;
		Statistics.DescriptorAllocatedSetCount =
			AllocatedSetCount >= Statistics.DescriptorAllocatedSetCount
			? 0 : Statistics.DescriptorAllocatedSetCount - AllocatedSetCount;
	}

	auto FVulkanMemoryBaselineTracker::RecordDescriptorPoolReset(
		uint64 ReleasedSetCount) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DescriptorAllocatedSetCount =
			ReleasedSetCount >= Statistics.DescriptorAllocatedSetCount
			? 0 : Statistics.DescriptorAllocatedSetCount - ReleasedSetCount;
	}

	auto FVulkanMemoryBaselineTracker::RecordDeferredDeleteEnqueued() -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DeferredDeletePendingCount = SaturatingAdd(
			Statistics.DeferredDeletePendingCount, 1);
		Statistics.DeferredDeleteHighWater = std::max(
			Statistics.DeferredDeleteHighWater,
			Statistics.DeferredDeletePendingCount);
	}

	auto FVulkanMemoryBaselineTracker::RecordDeferredDeletesReleased(
		uint64 Count, uint64 MaxAgeFrames) -> void
	{
		std::lock_guard Lock(Mutex);
		Statistics.DeferredDeletePendingCount =
			Count >= Statistics.DeferredDeletePendingCount
			? 0 : Statistics.DeferredDeletePendingCount - Count;
		Statistics.DeferredDeleteReleasedCount = SaturatingAdd(
			Statistics.DeferredDeleteReleasedCount, Count);
		Statistics.DeferredDeleteMaxAgeFrames = std::max(
			Statistics.DeferredDeleteMaxAgeFrames, MaxAgeFrames);
	}

	auto FVulkanMemoryBaselineTracker::RecordHeapBudgets(
		std::span<const uint64> UsageBytes,
		std::span<const uint64> BudgetBytes) -> void
	{
		std::lock_guard Lock(Mutex);
		const uint32 HeapCount = static_cast<uint32>(std::min({
			UsageBytes.size(), BudgetBytes.size(),
			static_cast<size_t>(FVulkanMemoryBaselineStatistics::MaxMemoryHeaps)}));
		Statistics.HeapCount = HeapCount;
		for (uint32 HeapIndex = 0; HeapIndex < HeapCount; ++HeapIndex)
		{
			Statistics.HeapUsageBytes[HeapIndex] = UsageBytes[HeapIndex];
			Statistics.HeapBudgetBytes[HeapIndex] = BudgetBytes[HeapIndex];
		}
	}

	auto FVulkanMemoryBaselineTracker::Snapshot() const
		-> FVulkanMemoryBaselineStatistics
	{
		std::lock_guard Lock(Mutex);
		return Statistics;
	}

	auto FVulkanMemoryBaselineTracker::ResetCounters() -> void
	{
		std::lock_guard Lock(Mutex);
		struct FLiveClass
		{
			uint64 AllocationCount = 0;
			uint64 AllocationBytes = 0;
			uint64 ArenaCapacityBytes = 0;
			uint64 ArenaLiveBytes = 0;
		};
		std::array<FLiveClass,
			FVulkanMemoryBaselineStatistics::AllocationClassCount> LiveClasses{};
		for (uint32 ClassIndex = 0; ClassIndex < LiveClasses.size(); ++ClassIndex)
		{
			LiveClasses[ClassIndex] = {
				Statistics.AllocationClasses[ClassIndex].LiveAllocationCount,
				Statistics.AllocationClasses[ClassIndex].LiveBytes,
				Statistics.AllocationClasses[ClassIndex].ArenaCapacityBytes,
				Statistics.AllocationClasses[ClassIndex].ArenaLiveBytes};
		}
		const uint64 DescriptorPoolCount = Statistics.DescriptorPoolCount;
		const uint64 DescriptorPoolSetCapacity =
			Statistics.DescriptorPoolSetCapacity;
		const uint64 DescriptorAllocatedSetCount =
			Statistics.DescriptorAllocatedSetCount;
		const uint64 DeferredDeletePendingCount =
			Statistics.DeferredDeletePendingCount;
		const uint32 HeapCount = Statistics.HeapCount;
		const auto HeapUsageBytes = Statistics.HeapUsageBytes;
		const auto HeapBudgetBytes = Statistics.HeapBudgetBytes;
		Statistics = {};
		for (uint32 ClassIndex = 0; ClassIndex < LiveClasses.size(); ++ClassIndex)
		{
			auto& Class = Statistics.AllocationClasses[ClassIndex];
			Class.LiveAllocationCount = LiveClasses[ClassIndex].AllocationCount;
			Class.LiveBytes = LiveClasses[ClassIndex].AllocationBytes;
			Class.PeakLiveBytes = Class.LiveBytes;
			Class.ArenaCapacityBytes = LiveClasses[ClassIndex].ArenaCapacityBytes;
			Class.ArenaLiveBytes = LiveClasses[ClassIndex].ArenaLiveBytes;
			Class.ArenaHighWaterBytes = Class.ArenaLiveBytes;
		}
		Statistics.DescriptorPoolCount = DescriptorPoolCount;
		Statistics.DescriptorPoolSetCapacity = DescriptorPoolSetCapacity;
		Statistics.DescriptorAllocatedSetCount = DescriptorAllocatedSetCount;
		Statistics.DescriptorPeakAllocatedSetCount = DescriptorAllocatedSetCount;
		Statistics.DeferredDeletePendingCount = DeferredDeletePendingCount;
		Statistics.DeferredDeleteHighWater = DeferredDeletePendingCount;
		Statistics.HeapCount = HeapCount;
		Statistics.HeapUsageBytes = HeapUsageBytes;
		Statistics.HeapBudgetBytes = HeapBudgetBytes;
		CurrentFrameUploadBytes = 0;
		CurrentFrameReadbackBytes = 0;
	}

	auto GetVulkanMemoryBaselineStatistics()
		-> FVulkanMemoryBaselineStatistics
	{
		return GVulkanMemoryBaselineTracker.Snapshot();
	}

	auto ResetVulkanMemoryBaselineStatistics() -> void
	{
		GVulkanMemoryBaselineTracker.ResetCounters();
	}

	auto FormatVulkanMemoryBaselineStatistics(
		const FVulkanMemoryBaselineStatistics& Statistics) -> std::string
	{
		std::string Classes;
		std::string Arenas;
		for (uint32 ClassIndex = 0;
			ClassIndex < FVulkanMemoryBaselineStatistics::AllocationClassCount;
			++ClassIndex)
		{
			const auto& Class = Statistics.AllocationClasses[ClassIndex];
			std::string Buckets;
			for (uint32 BucketIndex = 0;
				BucketIndex < FVulkanAllocationCandidateStatistics::SizeBucketCount;
				++BucketIndex)
			{
				Buckets += std::format("{}{}", BucketIndex == 0 ? "" : ".",
					Class.SizeBuckets[BucketIndex]);
			}
			Classes += std::format("{}{}:{}/{}/{}:[{}]",
				ClassIndex == 0 ? "" : ",", ClassIndex, Class.AllocationCount,
				Class.RequestedBytes, Class.PeakAllocationBytes, Buckets);
			Arenas += std::format("{}{}:{}/{}/{}/{}/{}/{}/{}",
				ClassIndex == 0 ? "" : ",", ClassIndex,
				Class.ArenaCapacityBytes, Class.ArenaLiveBytes,
				Class.ArenaHighWaterBytes, Class.ArenaReuseCount,
				Class.ArenaOverflowCount, Class.ArenaOversizeCount,
				Class.ArenaWaitCount);
		}
		std::string Heaps;
		for (uint32 HeapIndex = 0; HeapIndex < Statistics.HeapCount; ++HeapIndex)
		{
			Heaps += std::format("{}{}:{}/{}", HeapIndex == 0 ? "" : ",",
				HeapIndex, Statistics.HeapUsageBytes[HeapIndex],
				Statistics.HeapBudgetBytes[HeapIndex]);
		}
		return std::format(
			"classes[count/requested/peak:buckets4KiBx2]=[{}] "
			"arena[capacity/live/peak/reuse/overflow/oversize/wait]=[{}] "
			"upload[ops/bytes/framePeak]={}/{}/{} "
			"readback[ops/bytes/framePeak]={}/{}/{} frameWait[count/ns]={}/{} "
			"command[alloc/reuse]={}/{} descriptor[pools/cap/live/peak]={}/{}/{}/{} "
			"delete[pending/peak/released/maxAge]={}/{}/{}/{} heaps[usage/budget]=[{}]",
			Classes, Arenas, Statistics.UploadOperationCount, Statistics.UploadBytes,
			Statistics.PeakUploadBytesPerFrame, Statistics.ReadbackOperationCount,
			Statistics.ReadbackBytes, Statistics.PeakReadbackBytesPerFrame,
			Statistics.FrameFenceWaitCount, Statistics.FrameFenceWaitNanoseconds,
			Statistics.CommandBufferAllocationCount,
			Statistics.CommandBufferReuseCount, Statistics.DescriptorPoolCount,
			Statistics.DescriptorPoolSetCapacity,
			Statistics.DescriptorAllocatedSetCount,
			Statistics.DescriptorPeakAllocatedSetCount,
			Statistics.DeferredDeletePendingCount,
			Statistics.DeferredDeleteHighWater,
			Statistics.DeferredDeleteReleasedCount,
			Statistics.DeferredDeleteMaxAgeFrames, Heaps);
	}

	auto GetRHIMemoryStatistics() -> FRHIMemoryStatistics
	{
		const FVulkanMemoryBaselineStatistics Source =
			GVulkanMemoryBaselineTracker.Snapshot();
		FRHIMemoryStatistics Result;
		for (uint32 ClassIndex = 0; ClassIndex < Result.Classes.size(); ++ClassIndex)
		{
			const auto& Input = Source.AllocationClasses[ClassIndex];
			auto& Output = Result.Classes[ClassIndex];
			Output.LiveAllocationCount = Input.LiveAllocationCount;
			Output.LiveBytes = Input.LiveBytes;
			Output.PeakLiveBytes = Input.PeakLiveBytes;
			Output.AllocationCount = Input.AllocationCount;
			Output.AllocationBytes = Input.ActualBytes;
			Output.AllocationFailureCount = Input.AllocationFailureCount;
			Output.DedicatedAllocationCount = Input.DedicatedAllocationCount;
			Output.PeakAllocationBytes = Input.PeakAllocationBytes;
			Output.ArenaCapacityBytes = Input.ArenaCapacityBytes;
			Output.ArenaLiveBytes = Input.ArenaLiveBytes;
			Output.ArenaHighWaterBytes = Input.ArenaHighWaterBytes;
			Output.ArenaReuseCount = Input.ArenaReuseCount;
			Output.ArenaOverflowCount = Input.ArenaOverflowCount;
			Output.ArenaOversizeCount = Input.ArenaOversizeCount;
			Output.ArenaWaitCount = Input.ArenaWaitCount;
		}
		Result.HeapCount = Source.HeapCount;
		for (uint32 HeapIndex = 0; HeapIndex < Result.HeapCount; ++HeapIndex)
		{
			Result.Heaps[HeapIndex] = {
				.UsageBytes = Source.HeapUsageBytes[HeapIndex],
				.BudgetBytes = Source.HeapBudgetBytes[HeapIndex]};
		}
		Result.UploadOperationCount = Source.UploadOperationCount;
		Result.UploadBytes = Source.UploadBytes;
		Result.PeakUploadBytesPerFrame = Source.PeakUploadBytesPerFrame;
		Result.ReadbackOperationCount = Source.ReadbackOperationCount;
		Result.ReadbackBytes = Source.ReadbackBytes;
		Result.PeakReadbackBytesPerFrame = Source.PeakReadbackBytesPerFrame;
		Result.GPUWaitCount = Source.FrameFenceWaitCount;
		Result.GPUWaitNanoseconds = Source.FrameFenceWaitNanoseconds;
		Result.RetirementPendingCount = Source.DeferredDeletePendingCount;
		Result.RetirementHighWater = Source.DeferredDeleteHighWater;
		Result.RetirementReleasedCount = Source.DeferredDeleteReleasedCount;
		Result.RetirementMaxTokenLag = Source.DeferredDeleteMaxAgeFrames;
		return Result;
	}
} // namespace Durin::VulkanRHI
