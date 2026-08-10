#pragma once

#include "DynamicRHI.h"
#include "VulkanRHIAPI.h"

namespace Durin::VulkanRHI
{
	// Selects the backend-private VMA placement and CPU access policy.
	enum class EVulkanAllocationClassCandidate : uint8
	{
		DeviceLocal,
		DynamicUpload,
		TransferUpload,
		TransferReadback,
		Count
	};

	struct FVulkanAllocationCandidateStatistics
	{
		static constexpr uint32 SizeBucketCount = 16;

		uint64 AllocationCount = 0;
		uint64 AllocationFailureCount = 0;
		uint64 DedicatedAllocationCount = 0;
		uint64 RequestedBytes = 0;
		uint64 ActualBytes = 0;
		uint64 LiveAllocationCount = 0;
		uint64 LiveBytes = 0;
		uint64 PeakLiveBytes = 0;
		uint64 PeakAllocationBytes = 0;
		uint64 ArenaCapacityBytes = 0;
		uint64 ArenaLiveBytes = 0;
		uint64 ArenaHighWaterBytes = 0;
		uint64 ArenaReuseCount = 0;
		uint64 ArenaOverflowCount = 0;
		uint64 ArenaOversizeCount = 0;
		uint64 ArenaWaitCount = 0;
		std::array<uint64, SizeBucketCount> SizeBuckets = {};
	};

	// Captures the pre-M4 allocation, transfer, wait, reuse, and retirement baseline.
	struct FVulkanMemoryBaselineStatistics
	{
		static constexpr uint32 AllocationClassCount =
			static_cast<uint32>(EVulkanAllocationClassCandidate::Count);
		static constexpr uint32 MaxMemoryHeaps = 16;

		std::array<FVulkanAllocationCandidateStatistics, AllocationClassCount>
			AllocationClasses = {};
		uint64 UploadOperationCount = 0;
		uint64 UploadBytes = 0;
		uint64 PeakUploadBytesPerFrame = 0;
		uint64 ReadbackOperationCount = 0;
		uint64 ReadbackBytes = 0;
		uint64 PeakReadbackBytesPerFrame = 0;
		uint64 FrameFenceWaitCount = 0;
		uint64 FrameFenceWaitNanoseconds = 0;
		uint64 CommandBufferAllocationCount = 0;
		uint64 CommandBufferReuseCount = 0;
		uint64 DescriptorPoolCount = 0;
		uint64 DescriptorPoolSetCapacity = 0;
		uint64 DescriptorAllocatedSetCount = 0;
		uint64 DescriptorPeakAllocatedSetCount = 0;
		uint64 DeferredDeletePendingCount = 0;
		uint64 DeferredDeleteHighWater = 0;
		uint64 DeferredDeleteReleasedCount = 0;
		uint64 DeferredDeleteMaxAgeFrames = 0;
		uint32 HeapCount = 0;
		std::array<uint64, MaxMemoryHeaps> HeapUsageBytes = {};
		std::array<uint64, MaxMemoryHeaps> HeapBudgetBytes = {};
	};

	// Serializes diagnostic updates from RHI and cross-thread deletion producers.
	class VULKANRHI_API FVulkanMemoryBaselineTracker
	{
	public:
		auto BeginFrame() -> void;
		auto RecordAllocation(EVulkanAllocationClassCandidate Candidate,
			uint64 RequestedBytes, uint64 ActualBytes, bool bDedicated) -> void;
		auto RecordAllocationFailure(EVulkanAllocationClassCandidate Candidate) -> void;
		auto RecordAllocationFreed(EVulkanAllocationClassCandidate Candidate,
			uint64 ActualBytes) -> void;
		auto RecordArenaPageAllocated(EVulkanAllocationClassCandidate Candidate,
			uint64 Bytes) -> void;
		auto RecordArenaPageFreed(EVulkanAllocationClassCandidate Candidate,
			uint64 Bytes) -> void;
		auto RecordArenaRangeAllocated(EVulkanAllocationClassCandidate Candidate,
			uint64 Bytes, bool bReuse, bool bOversize) -> void;
		auto RecordArenaRangeReclaimed(EVulkanAllocationClassCandidate Candidate,
			uint64 Bytes) -> void;
		auto RecordArenaOverflow(EVulkanAllocationClassCandidate Candidate) -> void;
		auto RecordArenaWait(EVulkanAllocationClassCandidate Candidate) -> void;
		auto RecordUpload(uint64 Bytes) -> void;
		auto RecordReadback(uint64 Bytes) -> void;
		auto RecordFrameFenceWait(uint64 Nanoseconds) -> void;
		auto RecordCommandBufferAllocation() -> void;
		auto RecordCommandBufferReuse() -> void;
		auto RecordDescriptorPoolCreated(uint64 SetCapacity) -> void;
		auto RecordDescriptorPoolDestroyed(
			uint64 SetCapacity, uint64 AllocatedSetCount) -> void;
		auto RecordDescriptorSetsAllocated(uint64 SetCount) -> void;
		auto RecordDescriptorPoolReset(uint64 ReleasedSetCount) -> void;
		auto RecordDeferredDeleteEnqueued() -> void;
		auto RecordDeferredDeletesReleased(uint64 Count, uint64 MaxAgeFrames) -> void;
		auto RecordHeapBudgets(std::span<const uint64> UsageBytes,
			std::span<const uint64> BudgetBytes) -> void;

		auto Snapshot() const -> FVulkanMemoryBaselineStatistics;
		auto ResetCounters() -> void;

		static auto SaturatingAdd(uint64 Left, uint64 Right) -> uint64;

	private:
		mutable std::mutex Mutex;
		FVulkanMemoryBaselineStatistics Statistics;
		uint64 CurrentFrameUploadBytes = 0;
		uint64 CurrentFrameReadbackBytes = 0;
	};

	extern FVulkanMemoryBaselineTracker GVulkanMemoryBaselineTracker;

	VULKANRHI_API auto GetVulkanMemoryBaselineStatistics()
		-> FVulkanMemoryBaselineStatistics;
	VULKANRHI_API auto ResetVulkanMemoryBaselineStatistics() -> void;
	VULKANRHI_API auto FormatVulkanMemoryBaselineStatistics(
		const FVulkanMemoryBaselineStatistics& Statistics) -> std::string;
	VULKANRHI_API auto GetRHIMemoryStatistics() -> FRHIMemoryStatistics;
} // namespace Durin::VulkanRHI
