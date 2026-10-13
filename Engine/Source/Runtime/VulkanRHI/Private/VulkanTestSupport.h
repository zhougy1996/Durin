#pragma once

#include "CoreMinimal.h"
#include "VulkanPlatform.h"
#include "VulkanRHIAPI.h"
#include "RHIDefinitions.h"
#include "Backend/RHICompletionBackend.h"

namespace Durin::VulkanRHI
{
#if DURIN_VULKAN_TEST_SUPPORT
	struct FVulkanGraphicsPipelineTestStats
	{
		uint64 DestroyedPipelineCount = 0;
	};

	VULKANRHI_API auto GetVulkanGraphicsPipelineTestStats()
		-> FVulkanGraphicsPipelineTestStats;

	struct FVulkanStructuralCacheTestStats
	{
		uint64 RenderPassEntryCount = 0;
		uint64 FramebufferEntryCount = 0;
		uint64 DescriptorSetLayoutEntryCount = 0;
		uint64 PipelineLayoutEntryCount = 0;
		uint64 CreatedFramebufferViewCount = 0;
		uint64 ReleasedFramebufferViewCount = 0;
		uint64 CreatedFramebufferCount = 0;
		uint64 ReleasedFramebufferCount = 0;
	};

	extern std::atomic<uint64> GVulkanRenderPassEntryCount;
	// Successful native queue calls, independent of logical RDG batches or tokens.
	VULKANRHI_API auto GetVulkanNativeSubmissionCountForTesting() -> uint64;
	extern std::atomic<uint64> GVulkanFramebufferEntryCount;
	extern std::atomic<uint64> GVulkanDescriptorSetLayoutEntryCount;
	extern std::atomic<uint64> GVulkanPipelineLayoutEntryCount;
	extern std::atomic<uint64> GVulkanCreatedFramebufferViewCount;
	extern std::atomic<uint64> GVulkanReleasedFramebufferViewCount;
	extern std::atomic<uint64> GVulkanCreatedFramebufferCount;
	extern std::atomic<uint64> GVulkanReleasedFramebufferCount;
	VULKANRHI_API auto GetVulkanStructuralCacheTestStats()
		-> FVulkanStructuralCacheTestStats;

	struct FVulkanCompletionTestStats
	{
		uint64 LastReservedToken = 0;
		uint64 LastSubmittedToken = 0;
		uint64 CompletedToken = 0;
		uint64 PendingSubmissionCount = 0;
	};

	VULKANRHI_API auto GetLastVulkanSyncPointForTesting() -> FRHIGPUSyncPointRef;
	VULKANRHI_API auto TestVulkanSyncPointStorage() -> bool;
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
	VULKANRHI_API auto TestVulkanSubmissionBoundary() -> FVulkanSubmissionBoundaryTestResult;
	struct FVulkanSealedPressureTestResult
	{
		bool bMissingReservationPreserved = false;
		bool bOnlyRequiredPrefixSubmitted = false;
		bool bLeaseReleasedAfterWait = false;
		bool bActiveUsePreserved = false;
		bool bCyclePreserved = false;
	};
	VULKANRHI_API auto TestVulkanSealedPressure() -> FVulkanSealedPressureTestResult;
	VULKANRHI_API auto TestVulkanSealedPressureDependency() -> bool;
	VULKANRHI_API auto TestVulkanUniformBufferAdmission() -> bool;
	VULKANRHI_API auto ReserveVulkanUniformBufferBackingForTest(uint32 Size) -> std::shared_ptr<void>;
	struct FVulkanCrossQueueWaitTestResult
	{
		bool bConsumerBlocked = false;
		bool bFramePacingIndependent = false;
		bool bRetirementBlocked = false;
		bool bDescriptorReuseBlocked = false;
		bool bDescriptorReusedAfterCompletion = false;
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
	VULKANRHI_API auto RunVulkanCrossQueueWaitForTesting() -> FVulkanCrossQueueWaitTestResult;
	// Holds compute progress while Test runs on its calling thread. Native gate
	// operations execute through the RHI executor; Release is synchronous.
	VULKANRHI_API auto RunVulkanComputeGateForTesting(
		const std::function<void(const std::function<void()>&)>& Test) -> void;
	struct FVulkanQueueTransferTestResult
	{
		bool bBufferMatched = false;
		bool bTextureMatched = false;
		bool bRetainedUntilCompletion = false;
		bool bReleasedAfterCompletion = false;
		bool bUnselectedRangesPreserved = false;
	};
	VULKANRHI_API auto RunVulkanQueueTransferForTesting(bool bSynchronization2) -> FVulkanQueueTransferTestResult;
	VULKANRHI_API auto GetVulkanCompletionTestStats()
		-> FVulkanCompletionTestStats;
	struct FVulkanBackendPoolTestStats
	{
		std::array<uint64, FrameInFlight> DescriptorPoolTokens = {};
	};
	VULKANRHI_API auto GetVulkanBackendPoolTestStats()
		-> FVulkanBackendPoolTestStats;
	VULKANRHI_API auto SubmitAndRetireDescriptorPoolsForTesting() -> uint64;
	VULKANRHI_API auto WaitForAllVulkanSubmissionsForTesting() -> void;
	VULKANRHI_API auto ReleaseCompletedVulkanResourcesForTesting() -> void;

	struct FVulkanHotPathWorkTestStats
	{
		uint64 Sync2BufferBarriers = 0;
		uint64 LegacyBufferBarriers = 0;
		uint64 Sync2ImageBarriers = 0;
		uint64 LegacyImageBarriers = 0;
		uint64 BindingValidationVisits = 0;
		uint64 DescriptorDrawValidationVisits = 0;
		uint64 DescriptorSorts = 0;
		uint64 DescriptorHashes = 0;
		uint64 ViewportWrites = 0;
		uint64 ScissorWrites = 0;
		uint64 DepthBiasWrites = 0;
		uint64 PipelineBinds = 0;
		uint64 VertexBufferBinds = 0;
		uint64 IndexBufferBinds = 0;
		uint64 DescriptorBinds = 0;

		uint64 DescriptorOwnerRebuilds = 0;
		uint64 DescriptorOccupancyVerificationVisits = 0;
		uint64 DescriptorOccupancyMutations = 0;
	};
	VULKANRHI_API auto TestVulkanCommandBufferStateCache() -> bool;
	VULKANRHI_API auto ResetVulkanHotPathWorkTestStats() -> void;
	VULKANRHI_API auto GetVulkanHotPathWorkTestStats()
		-> FVulkanHotPathWorkTestStats;
	VULKANRHI_API auto SetVulkanBarrierPathOverrideForTest(
		std::optional<bool> bUseSynchronization2) -> void;
	VULKANRHI_API auto SelectVulkanSynchronization2BarrierPath(
		bool bSupportsSynchronization2) -> bool;
	extern std::atomic<uint64> GVulkanSync2BufferBarrierCount;
	extern std::atomic<uint64> GVulkanLegacyBufferBarrierCount;
	extern std::atomic<uint64> GVulkanSync2ImageBarrierCount;
	extern std::atomic<uint64> GVulkanLegacyImageBarrierCount;
	extern std::atomic<int32> GVulkanBarrierPathOverride;
	extern std::atomic<uint64> GVulkanBindingValidationVisitCount;
	extern std::atomic<uint64> GVulkanDescriptorDrawValidationVisitCount;
	extern std::atomic<uint64> GVulkanDescriptorSortCount;
	extern std::atomic<uint64> GVulkanDescriptorHashCount;
	extern std::atomic<uint64> GVulkanViewportWriteCount;
	extern std::atomic<uint64> GVulkanScissorWriteCount;
	extern std::atomic<uint64> GVulkanDepthBiasWriteCount;
	extern std::atomic<uint64> GVulkanPipelineBindCount;
	extern std::atomic<uint64> GVulkanVertexBufferBindCount;
	extern std::atomic<uint64> GVulkanIndexBufferBindCount;
	extern std::atomic<uint64> GVulkanDescriptorBindCount;
	extern std::atomic<uint64> GVulkanDescriptorOwnerRebuildCount;
	extern std::atomic<uint64> GVulkanDescriptorOccupancyVerificationVisitCount;
	extern std::atomic<uint64> GVulkanDescriptorOccupancyMutationCount;
#endif
} // namespace Durin::VulkanRHI
