#include "VulkanCommandBuffer.h"

#include "VulkanDevice.h"
#include "VulkanMemory.h"
#include "VulkanRenderPass.h"
#include "VulkanFramebuffer.h"
#include "VulkanContext.h"
#include "VulkanQueue.h"

namespace Doge::VulkanRHI
{
	FVulkanCommandBuffer::FVulkanCommandBuffer(FVulkanDevice& Device, FVulkanCommandBufferPool* Pool, bool bIsUploadOnly)
		: Device_(Device)
		, Pool_(Pool)
		, bIsUploadOnly_(bIsUploadOnly)
	{
		AllocMemory();
		Fence_ = Device.GetFenceManager().AllocateFence(false);
	}

	FVulkanCommandBuffer::~FVulkanCommandBuffer()
	{
	}

	auto FVulkanCommandBuffer::Begin() -> void
	{
		vk::CommandBufferBeginInfo BeginInfo;
		CommandBuffer_.begin(BeginInfo);
	}

	auto FVulkanCommandBuffer::End() -> void
	{
		CommandBuffer_.end();
	}

	auto FVulkanCommandBuffer::RefreshFenceStatus() -> void
	{
		if (State_ == EState::eSubmitted)
		{
			FVulkanFenceManager& FenceManager = Device_.GetFenceManager();
			if (FenceManager.IsFenceSignaled(Fence_))
			{
				State_ = EState::eNeedReset;
			}
		}
		else
		{
			check(!Fence_->IsSignaled());
		}
	}

	auto FVulkanCommandBuffer::AllocMemory() -> void
	{
		check(State_ == EState::eNotAllocated);

		vk::CommandBufferAllocateInfo AllocInfo;
		AllocInfo
			.setCommandPool(Pool_->GetHandle())
			.setCommandBufferCount(1)
			.setLevel(vk::CommandBufferLevel::ePrimary);

		CommandBuffer_ = Device_.GetHandle().allocateCommandBuffers(AllocInfo)[0];

		State_ = EState::eReadyForBegin;
	}

	auto FVulkanCommandBuffer::FreeMemory() -> void
	{
		check(State_ != EState::eNotAllocated);

		Device_.GetHandle().freeCommandBuffers(Pool_->GetHandle(), CommandBuffer_);

		State_ = EState::eNotAllocated;
	}

	auto FVulkanCommandBuffer::BeginRenderPass(FVulkanRenderPass* InRenderPass, FVulkanFramebuffer* InFramebuffer) -> void
	{
		vk::RenderPassBeginInfo BeginInfo;

		vk::ClearValue ClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}};

		BeginInfo
			.setRenderPass(InRenderPass->GetHandle())
			.setFramebuffer(InFramebuffer->GetHandle())
			.setRenderArea({{0, 0}, InFramebuffer->GetExtent()})
			.setClearValues(ClearColorValue);

		// Begin render pass
		CommandBuffer_.beginRenderPass(BeginInfo, vk::SubpassContents::eInline);
	}

	auto FVulkanCommandBuffer::EndRenderPass() -> void
	{
		CommandBuffer_.endRenderPass();
	}

	auto FVulkanCommandBuffer::IsSubmitted() const -> bool
	{
		return State_ == EState::eSubmitted;
	}

	auto FVulkanCommandBuffer::AddWaitSemaphore(FVulkanSemaphore* Semaphore) -> void
	{
		WaitSemaphores_.push_back(Semaphore);
	}

	auto FVulkanCommandBuffer::MarkSemaphoresAsSubmitted() -> void
	{
		WaitSemaphores_.clear();
	}

	FVulkanCommandBufferPool::FVulkanCommandBufferPool(FVulkanDevice& Device, FVulkanCommandBufferManager& Manager)
		: Device_(Device)
		, Manager_(Manager)
	{
	}

	auto FVulkanCommandBufferPool::CreatePool(uint32 QueueFamilyIndex) -> void
	{
		vk::CommandPoolCreateInfo CmdPoolInfo;
		CmdPoolInfo
			.setQueueFamilyIndex(QueueFamilyIndex)
			.setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);

		try
		{
			Handle_ = Device_.GetHandle().createCommandPool(CmdPoolInfo);
		}
		catch (const vk::SystemError& Error)
		{
			DOGE_ERROR("Vulkan", "Failed to create vulkan command pool: {}", Error.what());
		}
	}

	auto FVulkanCommandBufferPool::Create(bool bIsUploadOnly) -> FVulkanCommandBuffer*
	{
		if (!FreeCmdBuffers_.empty())
		{
			FVulkanCommandBuffer* CmdBuffer = FreeCmdBuffers_.back();
			FreeCmdBuffers_.pop_back();
			return CmdBuffer;
		}

		FVulkanCommandBuffer* CmdBuffer = new FVulkanCommandBuffer(Device_, this, bIsUploadOnly);
		CmdBuffers_.push_back(CmdBuffer);
		return CmdBuffer;
	}

	auto FVulkanCommandBufferPool::FreeUnusedCommandBuffers(FVulkanQueue* Queue) -> void
	{
		// Check if the command buffer is ready for begin or need reset, from end to begin
		for (int32 Index = static_cast<int32>(CmdBuffers_.size() - 1); Index >= 0; --Index)
		{
			FVulkanCommandBuffer* CmdBuffer = CmdBuffers_[Index];
			CmdBuffer->RefreshFenceStatus();
			if (CmdBuffer->State_ == FVulkanCommandBuffer::EState::eReadyForBegin || CmdBuffer->State_ == FVulkanCommandBuffer::EState::eNeedReset)
			{
				// remove at swap
				std::swap(CmdBuffers_[Index], CmdBuffers_.back());
				CmdBuffers_.pop_back();

				FreeCmdBuffers_.push_back(CmdBuffer);
			}
		}
	}

	FVulkanCommandBufferManager::FVulkanCommandBufferManager(FVulkanDevice& Device, FVulkanCommandListContext& Context)
		: Device_(Device)
		, Context_(Context)
		, Queue_(Context.GetQueue())
	{
		Pool_ = new FVulkanCommandBufferPool(Device_, *this);
		Pool_->CreatePool(Queue_->GetFamilyIndex());
		UploadCommandBuffer_ = Pool_->Create(false);
		ActiveCommandBuffer_ = Pool_->Create(false);
	}

	FVulkanCommandBufferManager::~FVulkanCommandBufferManager()
	{
		delete UploadCommandBuffer_;
		delete ActiveCommandBuffer_;
		delete Pool_;
	}

	auto FVulkanCommandBufferManager::SubmitActiveCmdBufferFromPresent(FVulkanSemaphore* SignalSemaphore) -> void
	{
		ActiveCommandBuffer_->End();
		FVulkanQueue* Queue = Context_.GetQueue();
		Queue->Submit(*ActiveCommandBuffer_, SignalSemaphore);

		ActiveCommandBuffer_ = nullptr;
		PrepareForNewActiveCommandBuffer();
	}

	auto FVulkanCommandBufferManager::PrepareForNewActiveCommandBuffer() -> void
	{
		ActiveCommandBuffer_ = Pool_->Create(false);
	}

	auto FVulkanCommandBufferManager::FreeUnusedCommandBuffers() -> void
	{
		Pool_->FreeUnusedCommandBuffers(Queue_);
	}
}