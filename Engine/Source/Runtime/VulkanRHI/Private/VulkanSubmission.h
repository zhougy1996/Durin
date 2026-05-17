#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanCommandBuffer;
	class FVulkanFence;
	class FVulkanQueue;
	class FVulkanSemaphore;

	class FVulkanPayload
	{
		friend class FVulkanQueue;
		friend class FVulkanCommandListContext;
		friend class FVulkanFrame;

	public:
		FVulkanPayload(FVulkanQueue& InQueue)
			: Queue(InQueue)
		{
		}

		~FVulkanPayload() = default;

	private:
		FVulkanQueue& Queue;

		std::vector<vk::PipelineStageFlags> WaitFlags; // Pipeline stages to wait on for each wait semaphore. Must match 1:1 with WaitSemaphores.
		std::vector<FVulkanSemaphore*> WaitSemaphores;

		std::vector<FVulkanCommandBuffer*> CommandBuffers;

		std::vector<FVulkanSemaphore*> SignalSemaphores;

		// Fence for this payload, will be signaled when the GPU finishes executing the command buffer associated with this payload
		FVulkanFence* Fence = nullptr;
	};

	class FVulkanFrame
	{
	public:
		explicit FVulkanFrame(FVulkanDevice& device);
		~FVulkanFrame();

		auto TrackInFlightPayload(std::vector<FVulkanPayload*>& Payload) -> void;

		auto Prepare() -> void;

		auto GetFrameFence() const -> FVulkanFence* { return FrameFence; }

	private:
		auto Reset() -> void;

		FVulkanDevice& Device;
		FVulkanFence* FrameFence = nullptr;
		std::vector<FVulkanPayload*> InFlightPayloads;
	};
}