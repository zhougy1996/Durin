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

		auto GetHandle() const -> vk::CommandBuffer { return CommandBuffer_; }

		auto GetFence() const -> FVulkanFence* { return Fence_; }

		auto IsSubmitted() const -> bool;

		auto AddWaitSemaphore(FVulkanSemaphore* Semaphore) -> void;

		enum class EState : uint8
		{
			eReadyForBegin,
			eIsInsideBegin,
			eIsInsideRenderPass,
			eHasEnded,
			eSubmitted,
			eNotAllocated,
			eNeedReset,
		};

	private:
		auto GetWaitSemaphores() const -> const std::vector<FVulkanSemaphore*>& { return WaitSemaphores_; }

		auto MarkSemaphoresAsSubmitted() -> void;

		FVulkanDevice& Device_;

		FVulkanCommandBufferPool* Pool_;

		EState State_ = EState::eNotAllocated;

		bool bIsUploadOnly_;

		vk::CommandBuffer CommandBuffer_;

		std::vector<FVulkanSemaphore*> WaitSemaphores_;

		FVulkanFence* Fence_;

		friend class FVulkanQueue;
		friend class FVulkanCommandBufferPool;
	};

	class FVulkanCommandBufferPool
	{
	public:
		FVulkanCommandBufferPool(FVulkanDevice& Device, FVulkanCommandBufferManager& Manager);

		auto GetHandle() const -> vk::CommandPool { return Handle_; }

		auto CreatePool(uint32 QueueFamilyIndex) -> void;

		auto Create(bool bIsUploadOnly) -> FVulkanCommandBuffer*;

		auto FreeUnusedCommandBuffers(FVulkanQueue* Queue) -> void;

	private:
		vk::CommandPool Handle_;

		FVulkanDevice& Device_;

		FVulkanCommandBufferManager& Manager_;

		std::vector<FVulkanCommandBuffer*> CmdBuffers_;

		std::vector<FVulkanCommandBuffer*> FreeCmdBuffers_;
	};

	class FVulkanCommandBufferManager
	{
	public:
		FVulkanCommandBufferManager(FVulkanDevice& Device, FVulkanCommandListContext& Context);

		~FVulkanCommandBufferManager();

		auto GetUploadCommandBuffer() -> FVulkanCommandBuffer* { return UploadCommandBuffer_; }

		auto GetActiveCommandBuffer() -> FVulkanCommandBuffer* { return ActiveCommandBuffer_; }

		auto SubmitActiveCmdBufferFromPresent(FVulkanSemaphore* SignalSemaphore) -> void;

		auto PrepareForNewActiveCommandBuffer() -> void;

		auto FreeUnusedCommandBuffers() -> void;

	private:
		FVulkanDevice& Device_;

		FVulkanCommandListContext& Context_;

		FVulkanQueue* Queue_;

		FVulkanCommandBufferPool* Pool_;

		FVulkanCommandBuffer* ActiveCommandBuffer_ = nullptr;

		FVulkanCommandBuffer* UploadCommandBuffer_ = nullptr;

		// Will be used to signal the rendering is done, mainly for upload command buffers
		// FVulkanSemaphore* ActiveCmdBufferSemaphore;
		//
		// std::vector<FVulkanSemaphore*> RenderingCompletedSemaphores_;
	};
}