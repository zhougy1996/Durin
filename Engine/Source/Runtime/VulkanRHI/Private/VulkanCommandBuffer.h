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
		FVulkanCommandBuffer(FVulkanDevice& Device, FVulkanCommandBufferPool* Pool, bool bIsUploadOnly);

		~FVulkanCommandBuffer();

		auto Begin() -> void;

		auto End() -> void;

		auto RefreshFenceStatus() -> void;

		auto AllocMemory() -> void;

		auto FreeMemory() -> void;

		auto BeginRenderPass(FVulkanRenderPass* InRenderPass, FVulkanFramebuffer* InFramebuffer) -> void;

		auto EndRenderPass() -> void;

		auto GetHandle() const -> vk::CommandBuffer { return CommandBuffer; }

		auto GetFence() const -> FVulkanFence* { return Fence_; }

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

		FVulkanDevice& Device;

		FVulkanCommandBufferPool* Pool;

		EState State_ = EState::NotAllocated;

		bool bIsUploadOnly;

		vk::CommandBuffer CommandBuffer;

		std::vector<FVulkanSemaphore*> WaitSemaphores;

		FVulkanFence* Fence_;

		friend class FVulkanQueue;
		friend class FVulkanCommandBufferPool;
	};

	class FVulkanCommandBufferPool
	{
	public:
		FVulkanCommandBufferPool(FVulkanDevice& InDevice, FVulkanCommandBufferManager& InManager);

		auto GetHandle() const -> vk::CommandPool { return Handle; }

		auto CreatePool(uint32 QueueFamilyIndex) -> void;

		auto Create(bool bIsUploadOnly) -> FVulkanCommandBuffer*;

		auto FreeUnusedCommandBuffers(FVulkanQueue* Queue) -> void;

	private:
		vk::CommandPool Handle;

		FVulkanDevice& Device;

		FVulkanCommandBufferManager& Manager;

		std::vector<FVulkanCommandBuffer*> CmdBuffers;

		std::vector<FVulkanCommandBuffer*> FreeCmdBuffers;
	};

	class FVulkanCommandBufferManager
	{
	public:
		FVulkanCommandBufferManager(FVulkanDevice& InDevice, FVulkanCommandListContext& InContext);

		~FVulkanCommandBufferManager();

		auto GetUploadCommandBuffer() const -> FVulkanCommandBuffer* { return UploadCommandBuffer; }

		auto GetActiveCommandBuffer() const -> FVulkanCommandBuffer* { return ActiveCommandBuffer; }

		auto SubmitActiveCmdBufferFromPresent(FVulkanSemaphore* SignalSemaphore) -> void;

		auto PrepareForNewActiveCommandBuffer() -> void;

		auto FreeUnusedCommandBuffers() -> void;

	private:
		FVulkanDevice& Device;

		FVulkanCommandListContext& Context;

		FVulkanQueue* Queue;

		FVulkanCommandBufferPool* Pool;

		FVulkanCommandBuffer* ActiveCommandBuffer = nullptr;

		FVulkanCommandBuffer* UploadCommandBuffer = nullptr;

		// Will be used to signal the rendering is done, mainly for upload command buffers
		// FVulkanSemaphore* ActiveCmdBufferSemaphore;
		//
		// std::vector<FVulkanSemaphore*> RenderingCompletedSemaphores;
	};
}