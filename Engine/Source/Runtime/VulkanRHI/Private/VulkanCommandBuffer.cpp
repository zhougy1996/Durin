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
		: Device(Device)
		, Pool(Pool)
		, bIsUploadOnly(bIsUploadOnly)
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
		CommandBuffer.begin(BeginInfo);
	}

	auto FVulkanCommandBuffer::End() -> void
	{
		CommandBuffer.end();
	}

	auto FVulkanCommandBuffer::RefreshFenceStatus() -> void
	{
		if (State_ == EState::Submitted)
		{
			FVulkanFenceManager& FenceManager = Device.GetFenceManager();
			if (FenceManager.IsFenceSignaled(Fence_))
			{
				State_ = EState::NeedReset;
			}
		}
		else
		{
			check(!Fence_->IsSignaled());
		}
	}

	auto FVulkanCommandBuffer::AllocMemory() -> void
	{
		check(State_ == EState::NotAllocated);

		vk::CommandBufferAllocateInfo AllocInfo;
		AllocInfo
			.setCommandPool(Pool->GetHandle())
			.setCommandBufferCount(1)
			.setLevel(vk::CommandBufferLevel::ePrimary);

		CommandBuffer = Device.GetHandle().allocateCommandBuffers(AllocInfo)[0];

		State_ = EState::ReadyForBegin;
	}

	auto FVulkanCommandBuffer::FreeMemory() -> void
	{
		check(State_ != EState::NotAllocated);

		Device.GetHandle().freeCommandBuffers(Pool->GetHandle(), CommandBuffer);

		State_ = EState::NotAllocated;
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
		CommandBuffer.beginRenderPass(BeginInfo, vk::SubpassContents::eInline);
	}

	auto FVulkanCommandBuffer::EndRenderPass() -> void
	{
		CommandBuffer.endRenderPass();
	}

	auto FVulkanCommandBuffer::IsSubmitted() const -> bool
	{
		return State_ == EState::Submitted;
	}

	auto FVulkanCommandBuffer::AddWaitSemaphore(FVulkanSemaphore* Semaphore) -> void
	{
		WaitSemaphores.push_back(Semaphore);
	}

	auto FVulkanCommandBuffer::MarkSemaphoresAsSubmitted() -> void
	{
		WaitSemaphores.clear();
	}

	FVulkanCommandBufferPool::FVulkanCommandBufferPool(FVulkanDevice& InDevice, FVulkanCommandBufferManager& InManager)
		: Device(InDevice)
		, Manager(InManager)
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
			Handle = Device.GetHandle().createCommandPool(CmdPoolInfo);
		}
		catch (const vk::SystemError& Error)
		{
			DOGE_ERROR("Vulkan", "Failed to create vulkan command pool: {}", Error.what());
		}
	}

	auto FVulkanCommandBufferPool::Create(bool bIsUploadOnly) -> FVulkanCommandBuffer*
	{
		if (!FreeCmdBuffers.empty())
		{
			FVulkanCommandBuffer* CmdBuffer = FreeCmdBuffers.back();
			FreeCmdBuffers.pop_back();
			return CmdBuffer;
		}

		FVulkanCommandBuffer* CmdBuffer = new FVulkanCommandBuffer(Device, this, bIsUploadOnly);
		CmdBuffers.push_back(CmdBuffer);
		return CmdBuffer;
	}

	auto FVulkanCommandBufferPool::FreeUnusedCommandBuffers(FVulkanQueue* Queue) -> void
	{
		// Check if the command buffer is ready for begin or need reset, from end to begin
		for (int32 Index = static_cast<int32>(CmdBuffers.size() - 1); Index >= 0; --Index)
		{
			FVulkanCommandBuffer* CmdBuffer = CmdBuffers[Index];
			CmdBuffer->RefreshFenceStatus();
			if (CmdBuffer->State_ == FVulkanCommandBuffer::EState::ReadyForBegin || CmdBuffer->State_ == FVulkanCommandBuffer::EState::NeedReset)
			{
				// remove at swap
				std::swap(CmdBuffers[Index], CmdBuffers.back());
				CmdBuffers.pop_back();

				FreeCmdBuffers.push_back(CmdBuffer);
			}
		}
	}

	FVulkanCommandBufferManager::FVulkanCommandBufferManager(FVulkanDevice& InDevice, FVulkanCommandListContext& InContext)
		: Device(InDevice)
		, Context(InContext)
		, Queue(InContext.GetQueue())
	{
		Pool = new FVulkanCommandBufferPool(Device, *this);
		Pool->CreatePool(Queue->GetFamilyIndex());
		UploadCommandBuffer = Pool->Create(false);
		ActiveCommandBuffer = Pool->Create(false);
	}

	FVulkanCommandBufferManager::~FVulkanCommandBufferManager()
	{
		delete UploadCommandBuffer;
		delete ActiveCommandBuffer;
		delete Pool;
	}

	auto FVulkanCommandBufferManager::SubmitActiveCmdBufferFromPresent(FVulkanSemaphore* SignalSemaphore) -> void
	{
		ActiveCommandBuffer->End();
		FVulkanQueue* Queue = Context.GetQueue();
		Queue->Submit(*ActiveCommandBuffer, SignalSemaphore);

		ActiveCommandBuffer = nullptr;
		PrepareForNewActiveCommandBuffer();
	}

	auto FVulkanCommandBufferManager::PrepareForNewActiveCommandBuffer() -> void
	{
		ActiveCommandBuffer = Pool->Create(false);
	}

	auto FVulkanCommandBufferManager::FreeUnusedCommandBuffers() -> void
	{
		Pool->FreeUnusedCommandBuffers(Queue);
	}
}