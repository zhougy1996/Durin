#include "VulkanCommandBuffer.h"

#include "VulkanDevice.h"
#include "VulkanMemory.h"
#include "VulkanRenderPass.h"
#include "VulkanFramebuffer.h"
#include "VulkanContext.h"
#include "VulkanQueue.h"

namespace Doge::VulkanRHI
{
	FVulkanCommandBuffer::FVulkanCommandBuffer(FVulkanDevice& InDevice, FVulkanCommandPool* InPool, bool bInIsUploadOnly)
		: Device(InDevice)
		, Pool(InPool)
		, bIsUploadOnly(bInIsUploadOnly)
	{
		AllocMemory();
		Fence = Device.GetFenceManager().AllocateFence(false);
	}

	FVulkanCommandBuffer::~FVulkanCommandBuffer()
	{
		Device.GetFenceManager().ReleaseFence(Fence);
		Device.GetHandle().freeCommandBuffers(Pool->GetHandle(), CommandBuffer);
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
		if (State == EState::Submitted)
		{
			FVulkanFenceManager& FenceManager = Device.GetFenceManager();
			if (FenceManager.IsFenceSignaled(Fence))
			{
				State = EState::NeedReset;
			}
		}
		else
		{
			check(!Fence->IsSignaled());
		}
	}

	auto FVulkanCommandBuffer::AllocMemory() -> void
	{
		check(State == EState::NotAllocated);

		vk::CommandBufferAllocateInfo AllocInfo;
		AllocInfo
			.setCommandPool(Pool->GetHandle())
			.setCommandBufferCount(1)
			.setLevel(vk::CommandBufferLevel::ePrimary);

		CommandBuffer = Device.GetHandle().allocateCommandBuffers(AllocInfo)[0];

		State = EState::ReadyForBegin;
	}

	auto FVulkanCommandBuffer::FreeMemory() -> void
	{
		check(State != EState::NotAllocated);

		Device.GetHandle().freeCommandBuffers(Pool->GetHandle(), CommandBuffer);

		State = EState::NotAllocated;
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
		return State == EState::Submitted;
	}

	auto FVulkanCommandBuffer::AddWaitSemaphore(FVulkanSemaphore* Semaphore) -> void
	{
		WaitSemaphores.push_back(Semaphore);
	}

	auto FVulkanCommandBuffer::MarkSemaphoresAsSubmitted() -> void
	{
		WaitSemaphores.clear();
	}

	FVulkanCommandPool::FVulkanCommandPool(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanCommandPool::~FVulkanCommandPool()
	{
		for (FVulkanCommandBuffer* CmdBuffer : CmdBuffers)
		{
			delete CmdBuffer;
		}

		for (FVulkanCommandBuffer* CmdBuffer : FreeCmdBuffers)
		{
			delete CmdBuffer;
		}

		Device.GetHandle().destroyCommandPool(Handle);
	}

	auto FVulkanCommandPool::CreatePool(uint32 QueueFamilyIndex) -> void
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

	auto FVulkanCommandPool::Create(bool bIsUploadOnly) -> FVulkanCommandBuffer*
	{
		if (!FreeCmdBuffers.empty())
		{
			FVulkanCommandBuffer* CmdBuffer = FreeCmdBuffers.back();
			FreeCmdBuffers.pop_back();
			CmdBuffers.push_back(CmdBuffer);
			return CmdBuffer;
		}

		FVulkanCommandBuffer* CmdBuffer = new FVulkanCommandBuffer(Device, this, bIsUploadOnly);
		CmdBuffers.push_back(CmdBuffer);
		return CmdBuffer;
	}

	auto FVulkanCommandPool::FreeUnusedCommandBuffers(FVulkanQueue* Queue) -> void
	{
		// Check if the command buffer is ready for begin or need reset, from end to begin
		for (int32 Index = static_cast<int32>(CmdBuffers.size() - 1); Index >= 0; --Index)
		{
			FVulkanCommandBuffer* CmdBuffer = CmdBuffers[Index];
			CmdBuffer->RefreshFenceStatus();
			if (CmdBuffer->State == FVulkanCommandBuffer::EState::ReadyForBegin || CmdBuffer->State == FVulkanCommandBuffer::EState::NeedReset)
			{
				// remove at swap
				std::swap(CmdBuffers[Index], CmdBuffers.back());
				CmdBuffers.pop_back();

				FreeCmdBuffers.push_back(CmdBuffer);
			}
		}
	}
}