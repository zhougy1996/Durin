#pragma once
#include "RHICompletion.h"

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandBuffer;
	class FVulkanFence;
	class FVulkanQueue;
	class FVulkanSemaphore;
	class FVulkanCommandListContext;
	class FVulkanGPUTimingQuery;

	// Owns one queue submission's command buffers, waits, signals, and completion fence.
	class FVulkanPayload
	{
		friend class FVulkanQueue;
		friend class FVulkanCommandListContext;
		friend class FVulkanFrame;
		friend class FVulkanCompletionTracker;
		friend class FVulkanSubmissionCoordinator;

	public:
		FVulkanPayload(FVulkanQueue& InQueue, FRHIGPUSyncPointRef InSyncPoint);
		~FVulkanPayload();
		FVulkanPayload(const FVulkanPayload&) = delete;
		auto operator=(const FVulkanPayload&) -> FVulkanPayload& = delete;
		auto GetSyncPoint() const -> const FRHIGPUSyncPointRef& { return SyncPoint; }
		auto AttachSignal(const FRHIGPUSyncPointRef& Signal) -> bool;
		auto RetainAllocation(std::shared_ptr<void> Owner) -> void
		{
			require(Owner);
			if (!std::ranges::contains(AllocationOwners, Owner))
				AllocationOwners.push_back(std::move(Owner));
		}
		auto AddCompletionWait(const FRHIGPUSyncPointRef& SyncPoint) -> void
		{
			// Preserve every success dependency until its authority/state is validated.
			if (!std::ranges::contains(CompletionWaits, SyncPoint))
				CompletionWaits.push_back(SyncPoint);
		}

	private:
		FVulkanQueue& Queue;
		FRHIGPUSyncPointRef SyncPoint;
		std::vector<FRHIGPUSyncPointRef> Signals;
		std::vector<std::shared_ptr<void>> AllocationOwners;
		std::vector<TRefCountPtr<FVulkanGPUTimingQuery>> TimingQueries;
		std::vector<std::shared_ptr<void>> ReplayStorageOwners;
		std::vector<std::shared_ptr<void>> RetainedTransitions;
		std::vector<FRHIGPUSyncPointRef> CompletionWaits;

		std::vector<vk::PipelineStageFlags> WaitFlags; // Pipeline stages to wait on for each wait semaphore. Must match 1:1 with WaitSemaphores.
		std::vector<FVulkanSemaphore*> WaitSemaphores;

		std::vector<FVulkanCommandBuffer*> CommandBuffers;

		std::vector<FVulkanSemaphore*> SignalSemaphores;

	};

	// Runs on the RHI thread and owns the native submission boundary.
	class FVulkanSubmissionCoordinator
	{
	public:
		explicit FVulkanSubmissionCoordinator(FVulkanDevice& InDevice) : Device(InDevice) {}
		auto EnqueueContext(FVulkanCommandListContext& Context) -> FRHIGPUSyncPointRef;
		auto DiscardPending() -> void;
		auto Submit(std::unique_ptr<FVulkanPayload> Payload) -> FRHIGPUSyncPointRef;
		auto SubmitBatch(std::vector<std::unique_ptr<FVulkanPayload>> Payloads) -> void;
		auto SubmitContext(FVulkanCommandListContext& Context) -> FRHIGPUSyncPointRef;
		auto SubmitPendingContexts(FVulkanCommandListContext* CallingContext = nullptr) -> void;
		auto GetAllocationUses(const std::shared_ptr<void>& Owner) const -> FRHIRetirementPrerequisites;
		auto WaitForAllocation(const std::weak_ptr<void>& Owner) -> void;
	private:
		auto SubmitNative(std::unique_ptr<FVulkanPayload> Payload) -> void;
		FVulkanDevice& Device;
		std::vector<std::unique_ptr<FVulkanPayload>> PendingPayloads;
	};

	// Retains submitted payloads until their GPU work completes and resources can recycle.
	class FVulkanFrame
	{
	public:
		explicit FVulkanFrame(FVulkanDevice& Device);
		~FVulkanFrame() = default;

		auto Prepare() -> void;

		auto SetRetirementUses(FRHIRetirementPrerequisites Uses) -> void;

	private:
		FVulkanDevice& Device;
		FRHIRetirementPrerequisites RetirementUses;
	};
}
