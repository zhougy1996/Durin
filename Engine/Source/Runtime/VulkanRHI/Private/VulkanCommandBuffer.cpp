#include "VulkanCommandBuffer.h"

#include "VulkanDevice.h"
#include "VulkanMemory.h"
#include "VulkanRenderPass.h"
#include "VulkanFramebuffer.h"

namespace Doge::VulkanRHI
{
	FVulkanCommandBuffer::FVulkanCommandBuffer(FVulkanDevice& InDevice, FVulkanCommandBufferPool* InPool)
		: Device(InDevice)
		, Pool(InPool)
	{
		AllocMemory();
	}

	FVulkanCommandBuffer::~FVulkanCommandBuffer()
	{
		if (State != EState::NotAllocated)
		{
			FreeMemory();
		}
	}

	auto FVulkanCommandBuffer::Begin() -> void
	{
		check(State == EState::ReadyForBegin);
		vk::CommandBufferBeginInfo BeginInfo;
		Handle.begin(BeginInfo);
		State = EState::IsInsideBegin;
	}

	auto FVulkanCommandBuffer::End() -> void
	{
		if (State == EState::HasEnded)
		{
			return;
		}
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		Handle.end();
		State = EState::HasEnded;
	}

	auto FVulkanCommandBuffer::AllocMemory() -> void
	{
		check(State == EState::NotAllocated);

		vk::CommandBufferAllocateInfo AllocInfo;
		AllocInfo
			.setCommandPool(Pool->GetHandle())
			.setCommandBufferCount(1)
			.setLevel(vk::CommandBufferLevel::ePrimary);

		Handle = Device.GetHandle().allocateCommandBuffers(AllocInfo)[0];

		State = EState::ReadyForBegin;
	}

	auto FVulkanCommandBuffer::FreeMemory() -> void
	{
		check(State != EState::NotAllocated);
		check(Handle != VK_NULL_HANDLE);
		Device.GetHandle().freeCommandBuffers(Pool->GetHandle(), Handle);
		State = EState::NotAllocated;
	}

	auto FVulkanCommandBuffer::Reset() -> void
	{
		check(State != EState::NotAllocated);
		if (State == EState::ReadyForBegin)
		{
			return;
		}
		check(Handle != nullptr);
		vk::CommandBufferResetFlags ResetFlags = vk::CommandBufferResetFlagBits::eReleaseResources;
		Handle.reset(ResetFlags);
		State = EState::ReadyForBegin;
	}

	auto FVulkanCommandBuffer::SetSubmitted() -> void
	{
		check(State == EState::HasEnded);
		State = EState::Submitted;
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
		Handle.beginRenderPass(BeginInfo, vk::SubpassContents::eInline);
	}

	auto FVulkanCommandBuffer::EndRenderPass() -> void
	{
		Handle.endRenderPass();
	}

	auto FVulkanCommandBuffer::IsSubmitted() const -> bool
	{
		return State == EState::Submitted;
	}

	FVulkanCommandBufferPool::FVulkanCommandBufferPool(FVulkanDevice& InDevice)
		: Device(InDevice)
	{
	}

	FVulkanCommandBufferPool::~FVulkanCommandBufferPool()
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

	auto FVulkanCommandBufferPool::CreatePool(uint32 QueueFamilyIndex) -> void
	{
		vk::CommandPoolCreateInfo CmdPoolInfo;
		CmdPoolInfo
			.setQueueFamilyIndex(QueueFamilyIndex)
			.setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);

		Handle = Device.GetHandle().createCommandPool(CmdPoolInfo);
	}

	auto FVulkanCommandBufferPool::Create() -> FVulkanCommandBuffer*
	{
		FVulkanCommandBuffer* CmdBuffer = nullptr;

		if (!FreeCmdBuffers.empty())
		{
			CmdBuffer = FreeCmdBuffers.back();
			FreeCmdBuffers.pop_back();
			CmdBuffers.push_back(CmdBuffer);

			return CmdBuffer;
		}

		CmdBuffer = new FVulkanCommandBuffer(Device, this);
		CmdBuffers.push_back(CmdBuffer);
		return CmdBuffer;
	}

	auto FVulkanCommandBufferPool::FreeUnusedCommandBuffers(FVulkanQueue* Queue) -> void
	{
		auto RangeToFree = std::ranges::partition(CmdBuffers, [](FVulkanCommandBuffer* CmdBuffer) {
			if (CmdBuffer->State == FVulkanCommandBuffer::EState::ReadyForBegin || CmdBuffer->State == FVulkanCommandBuffer::EState::NeedReset)
			{
				CmdBuffer->Reset();
				check(CmdBuffer->IsReadyForBegin());
				return false;
			}
			return true;
		});

		FreeCmdBuffers.insert(FreeCmdBuffers.end(), RangeToFree.begin(), RangeToFree.end());
		CmdBuffers.erase(RangeToFree.begin(), RangeToFree.end());
	}
}
