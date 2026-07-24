#pragma once

namespace Durin::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanRenderPass;
	class FVulkanFramebuffer;
	class FVulkanCommandListContext;
	class FVulkanSemaphore;

	class FVulkanCommandBufferPool;
	class FVulkanCommandBufferManager;

	// Tracks a Vulkan command buffer through recording, submission, and completion states.
	class FVulkanCommandBuffer
	{
	public:
		FVulkanCommandBuffer(FVulkanDevice& InDevice, FVulkanCommandBufferPool* InPool);

		~FVulkanCommandBuffer();

		auto Begin() -> void;

		auto End() -> void;

		auto Reset() -> void;

		auto SetSubmitted() -> void;

		auto BeginRenderPass(FVulkanRenderPass* InRenderPass, FVulkanFramebuffer* InFramebuffer, std::span<const vk::ClearValue> InClearValues, FName DebugName) -> void;

		auto EndRenderPass() -> void;

		auto GetHandle() const -> vk::CommandBuffer { return Handle; }

		auto IsSubmitted() const -> bool;

		auto IsReadyForBegin() const -> bool { return State == EState::ReadyForBegin; }

		// Enforces the legal recording and submission transitions of a command buffer.
		enum class EState : uint8
		{
			ReadyForBegin,
			IsInsideBegin,
			IsInsideRenderPass,
			HasEnded,
			Submitted,
			NotAllocated,
			NeedReset,
		};

	private:
		auto AllocMemory() -> void;

		auto FreeMemory() -> void;

		FVulkanDevice& Device;

		FVulkanCommandBufferPool* Pool;

		EState State = EState::NotAllocated;

		double SubmittedTime = 0.0;

		vk::CommandBuffer Handle;
		bool bRenderPassDebugLabelOpen = false;

		friend class FVulkanQueue;
		friend class FVulkanCommandBufferPool;
	};

	// Owns command buffers allocated from one Vulkan command pool.
	class FVulkanCommandBufferPool
	{
	public:
		FVulkanCommandBufferPool(FVulkanDevice& InDevice);
		~FVulkanCommandBufferPool();

		auto GetHandle() const -> vk::CommandPool { return Handle; }

		auto CreatePool(uint32 QueueFamilyIndex) -> void;

		auto Create() -> FVulkanCommandBuffer*;

		auto FreeUnusedCommandBuffers(FVulkanQueue* Queue) -> void;

	private:
		vk::CommandPool Handle;

		FVulkanDevice& Device;

		std::vector<FVulkanCommandBuffer*> CmdBuffers;

		std::vector<FVulkanCommandBuffer*> FreeCmdBuffers;
	};
}
