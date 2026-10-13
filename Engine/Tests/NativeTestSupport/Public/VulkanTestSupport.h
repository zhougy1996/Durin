#pragma once

#include "CoreMinimal.h"
#include "VulkanPlatform.h"
#include "VulkanGPUTiming.h"
#include "RHIDefinitions.h"
#include "Backend/RHICompletionBackend.h"

namespace Durin::VulkanRHI
{
	auto GetVulkanDeviceForTest(FVulkanDynamicRHI& RHI) -> FVulkanDevice*;
	auto PollVulkanGPUTimingForTest(FVulkanDynamicRHI& RHI) -> void;
	auto GetVulkanGPUTimingStatisticsForTest(FVulkanDynamicRHI& RHI) -> FVulkanGPUTimingStatistics;
	auto GetVulkanDeviceDescriptionForTiming() -> std::string;
	auto GetVulkanDeviceLuidForTiming() -> std::optional<std::array<std::byte, 8>>;
	struct FVulkanCompletionTestStats
	{
		uint64 LastReservedToken = 0;
		uint64 LastSubmittedToken = 0;
		uint64 CompletedToken = 0;
		uint64 PendingSubmissionCount = 0;
	};

	auto GetLastVulkanSyncPointForTesting() -> FRHIGPUSyncPointRef;
	auto TestVulkanSyncPointStorage() -> bool;
	struct FVulkanSubmissionBoundaryTestResult
	{
		bool bPayloadUsesExplicitSyncPoint = false;
		bool bLogicalSignalUsesRecordingSyncPoint = false;
		bool bQueuedStorageRetired = false;
		bool bQueuedStorageDiscarded = false;
		bool bSealDidNotSubmit = false;
		bool bEarlierSyncPointSubmitted = false;
		bool bDiscardCanceled = false;
		bool bStorageRetained = false;
		bool bStorageReleased = false;
		bool bAllocationRetained = false;
		bool bAllocationReturned = false;
		bool bTimingDiscarded = false;
	};
	auto TestVulkanSubmissionBoundary() -> FVulkanSubmissionBoundaryTestResult;
	struct FVulkanSealedPressureTestResult
	{
		bool bMissingReservationPreserved = false;
		bool bOnlyRequiredPrefixSubmitted = false;
		bool bLeaseReleasedAfterWait = false;
		bool bActiveUsePreserved = false;
	};
	auto TestVulkanSealedPressure() -> FVulkanSealedPressureTestResult;
	auto TestVulkanSealedPressureDependency() -> bool;
	auto TestVulkanUniformBufferAdmission() -> bool;
	auto ReserveVulkanUniformBufferBackingForTest(uint32 Size) -> std::shared_ptr<void>;
	struct FVulkanCrossQueueWaitTestResult
	{
		bool bConsumerBlocked = false;
		bool bFramePacingIndependent = false;
		bool bRetirementBlocked = false;
		bool bUniformReuseBlocked = false;
		bool bUniformReusedAfterCompletion = false;
		bool bTimingSupported = false;
		bool bTimingBlocked = false;
		bool bTimingCompleted = false;
		bool bTransferReuseBlocked = false;
		bool bTransferReusedAfterCompletion = false;
		bool bBatchOrdered = false;
		bool bBatchCycleRejected = false;
		bool bBatchMissingProducerRejected = false;
		bool bBatchMissingReservationRejected = false;
		bool bCompleted = false;
		FRHIGPUCompletionPoint Producer;
		FRHIGPUCompletionPoint Consumer;
	};
	auto RunVulkanCrossQueueWaitForTesting() -> FVulkanCrossQueueWaitTestResult;
	// Holds compute progress while Test runs on its calling thread. Native gate
	// operations execute through the RHI executor; Release is synchronous.
	auto RunVulkanComputeGateForTesting(
		const std::function<void(const std::function<void()>&)>& Test) -> void;
	struct FVulkanQueueTransferTestResult
	{
		bool bBufferMatched = false;
		bool bTextureMatched = false;
		bool bRetainedUntilCompletion = false;
		bool bReleasedAfterCompletion = false;
		bool bUnselectedRangesPreserved = false;
	};
	auto RunVulkanQueueTransferForTesting(bool bSynchronization2) -> FVulkanQueueTransferTestResult;
	auto GetVulkanCompletionTestStats()
		-> FVulkanCompletionTestStats;
	auto SubmitAndRetireDescriptorPoolsForTesting() -> uint64;
	auto WaitForAllVulkanSubmissionsForTesting() -> void;
	auto ReleaseCompletedVulkanResourcesForTesting() -> void;

} // namespace Durin::VulkanRHI
