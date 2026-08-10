#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandBuffer;
	class FVulkanFence;
	class FVulkanQueue;
	class FVulkanSemaphore;

	// Owns one queue submission's command buffers, waits, signals, and completion fence.
	class FVulkanPayload
	{
		friend class FVulkanQueue;
		friend class FVulkanCommandListContext;
		friend class FVulkanFrame;
		friend class FVulkanCompletionTracker;

	public:
		FVulkanPayload(FVulkanQueue& InQueue, uint64 InToken)
			: Queue(InQueue)
			, Token(InToken)
		{
		}

		~FVulkanPayload() = default;

	private:
		FVulkanQueue& Queue;
		uint64 Token = 0;

		std::vector<vk::PipelineStageFlags> WaitFlags; // Pipeline stages to wait on for each wait semaphore. Must match 1:1 with WaitSemaphores.
		std::vector<FVulkanSemaphore*> WaitSemaphores;

		std::vector<FVulkanCommandBuffer*> CommandBuffers;

		std::vector<FVulkanSemaphore*> SignalSemaphores;

	};

	// Retains submitted payloads until their GPU work completes and resources can recycle.
	class FVulkanFrame
	{
	public:
		explicit FVulkanFrame(FVulkanDevice& Device);
		~FVulkanFrame() = default;

		auto Prepare() -> void;

		auto SetLastSubmittedToken(uint64 Token) -> void;

	private:
		FVulkanDevice& Device;
		uint64 LastSubmittedToken = 0;
	};
}
