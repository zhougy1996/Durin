#pragma once

namespace Doge::VulkanRHI
{
	class FVulkanDevice;
	class FVulkanRenderPass;
	class FVulkanFramebuffer;
	class FVulkanCommandListContext;
	class FVulkanSemaphore;
	class FVulkanQueue;
	class FVulkanFence;

	class FVulkanCommandBufferPool;
	class FVulkanCommandBufferManager;

	class FVulkanCommandBuffer
	{
	public:
		FVulkanCommandBuffer(FVulkanDevice& InDevice, FVulkanCommandBufferPool* InPool);

		~FVulkanCommandBuffer();

		auto Begin() -> void;

		auto End() -> void;

		auto RefreshFenceStatus() -> void;

		auto Reset() -> void;

		auto SetSubmitted() -> void;

		auto BeginRenderPass(FVulkanRenderPass* InRenderPass, FVulkanFramebuffer* InFramebuffer) -> void;

		auto EndRenderPass() -> void;

		auto GetHandle() const -> vk::CommandBuffer { return Handle; }

		auto GetFence() const -> FVulkanFence* { return Fence; }

		auto IsSubmitted() const -> bool;

		auto AddWaitSemaphore(FVulkanSemaphore* Semaphore) -> void;

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
		auto GetWaitSemaphores() const -> const std::vector<FVulkanSemaphore*>& { return WaitSemaphores; }

		auto MarkSemaphoresAsSubmitted() -> void;

		auto AllocMemory() -> void;

		auto FreeMemory() -> void;

		FVulkanDevice& Device;

		FVulkanCommandBufferPool* Pool;

		EState State = EState::NotAllocated;

		vk::CommandBuffer Handle;

		std::vector<FVulkanSemaphore*> WaitSemaphores;

		FVulkanFence* Fence;

		friend class FVulkanQueue;
		friend class FVulkanCommandBufferPool;
	};

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