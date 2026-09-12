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
		FVulkanPayload(FVulkanQueue& InQueue, uint64 InToken);
		~FVulkanPayload();
		FVulkanPayload(const FVulkanPayload&) = delete;
		auto operator=(const FVulkanPayload&) -> FVulkanPayload& = delete;
		auto GetTicket() const -> const FRHIGPUSubmissionTicket& { return Ticket; }
		auto RetainAllocation(std::shared_ptr<void> Owner) -> void
		{
			require(Owner);
			if (std::ranges::find(AllocationOwners, Owner) == AllocationOwners.end())
				AllocationOwners.push_back(std::move(Owner));
		}
		auto AddCompletionWait(const FRHIGPUSubmissionTicket& Ticket) -> void
		{
			// Preserve every success dependency until its authority/state is validated.
			CompletionWaits.push_back(Ticket);
		}

	private:
		FVulkanQueue& Queue;
		uint64 Token = 0;
		FRHIGPUSubmissionTicket Ticket;
		std::vector<std::shared_ptr<void>> AllocationOwners;
		std::vector<TRefCountPtr<FVulkanGPUTimingQuery>> TimingQueries;
		std::vector<std::shared_ptr<void>> ReplayStorageOwners;
		std::vector<std::shared_ptr<void>> RetainedTransitions;
		std::vector<FRHIGPUSubmissionTicket> CompletionWaits;

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
		auto EnqueueContext(FVulkanCommandListContext& Context) -> FRHIGPUSubmissionTicket;
		auto DiscardPending() -> void;
		auto Submit(std::unique_ptr<FVulkanPayload> Payload) -> FRHIGPUSubmissionTicket;
		auto SubmitBatch(std::vector<std::unique_ptr<FVulkanPayload>> Payloads) -> void;
		auto SubmitContext(FVulkanCommandListContext& Context) -> FRHIGPUSubmissionTicket;
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
