#include "VulkanCommandBuffer.h"

#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDiagnostics.h"
#include "VulkanMemory.h"
#include "VulkanRenderPass.h"
#include "VulkanFramebuffer.h"
#include "VulkanRHIPrivate.h"

namespace Durin::VulkanRHI
{
	FVulkanCommandBuffer::FVulkanCommandBuffer(FVulkanDevice& InDevice, FVulkanCommandBufferPool* InPool)
		: Device(InDevice)
		, Pool(InPool)
	{
		CheckVulkanRHIThread();
		AllocMemory();
	}

	FVulkanCommandBuffer::~FVulkanCommandBuffer()
	{
		CheckVulkanRHIThread();
		if (State != EState::NotAllocated)
		{
			FreeMemory();
		}
	}

	auto FVulkanCommandBuffer::Begin() -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::ReadyForBegin);
		vk::CommandBufferBeginInfo BeginInfo;
		Handle.begin(BeginInfo);
		State = EState::IsInsideBegin;
	}

	auto FVulkanCommandBuffer::End() -> void
	{
		CheckVulkanRHIThread();
		if (State == EState::HasEnded)
		{
			return;
		}
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		check(DiagnosticRegionDepth == 0);
		Handle.end();
		State = EState::HasEnded;
	}

	auto FVulkanCommandBuffer::AllocMemory() -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::NotAllocated);

		vk::CommandBufferAllocateInfo AllocInfo;
		AllocInfo
			.setCommandPool(Pool->GetHandle())
			.setCommandBufferCount(1)
			.setLevel(vk::CommandBufferLevel::ePrimary);

		Handle = Device.GetHandle().allocateCommandBuffers(AllocInfo)[0];
		Device.GetRHI().GetDebugUtils().NameObject(Handle,
			Device.GetRHI().GetDebugUtils().MakeInternalName("CommandBuffer"));

		State = EState::ReadyForBegin;
	}

	auto FVulkanCommandBuffer::FreeMemory() -> void
	{
		CheckVulkanRHIThread();
		check(State != EState::NotAllocated);
		check(Handle != VK_NULL_HANDLE);
		Device.GetHandle().freeCommandBuffers(Pool->GetHandle(), Handle);
		State = EState::NotAllocated;
	}

	auto FVulkanCommandBuffer::Reset() -> void
	{
		CheckVulkanRHIThread();
		check(State != EState::NotAllocated);
		if (State == EState::ReadyForBegin)
		{
			return;
		}
		check(Handle != nullptr);
		vk::CommandBufferResetFlags ResetFlags = vk::CommandBufferResetFlagBits::eReleaseResources;
		Handle.reset(ResetFlags);
		DiagnosticRegionDepth = 0;
		State = EState::ReadyForBegin;
	}

	auto FVulkanCommandBuffer::SetSubmitted() -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::HasEnded);
		State = EState::Submitted;
	}

	auto FVulkanCommandBuffer::BeginRenderPass(FVulkanRenderPass* InRenderPass, FVulkanFramebuffer* InFramebuffer, std::span<const vk::ClearValue> InClearValues, FName DebugName) -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::IsInsideBegin);
		if (!DebugName.IsNone())
		{
			const std::string LabelName = DebugName.ToString();
			bRenderPassDebugLabelOpen =
				Device.GetRHI().GetDebugUtils().BeginLabel(Handle, LabelName);
		}
		vk::RenderPassBeginInfo BeginInfo;

		BeginInfo
			.setRenderPass(InRenderPass->GetHandle())
			.setFramebuffer(InFramebuffer->GetHandle())
			.setRenderArea({{0, 0}, InFramebuffer->GetExtent()})
			.setClearValues(InClearValues);

		// Begin render pass
		Handle.beginRenderPass(BeginInfo, vk::SubpassContents::eInline);
		State = EState::IsInsideRenderPass;
	}

	auto FVulkanCommandBuffer::EndRenderPass() -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::IsInsideRenderPass);
		Handle.endRenderPass();
		if (bRenderPassDebugLabelOpen)
		{
			Device.GetRHI().GetDebugUtils().EndLabel(Handle);
			bRenderPassDebugLabelOpen = false;
		}
		State = EState::IsInsideBegin;
	}

	auto FVulkanCommandBuffer::BeginDiagnosticRegion(std::string_view Name) -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		if (Device.GetRHI().GetDebugUtils().BeginLabel(Handle, Name))
			++DiagnosticRegionDepth;
	}

	auto FVulkanCommandBuffer::EndDiagnosticRegion() -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		if (DiagnosticRegionDepth == 0) return;
		Device.GetRHI().GetDebugUtils().EndLabel(Handle);
		--DiagnosticRegionDepth;
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
		CheckVulkanRHIThread();
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
		CheckVulkanRHIThread();
		vk::CommandPoolCreateInfo CmdPoolInfo;
		CmdPoolInfo
			.setQueueFamilyIndex(QueueFamilyIndex)
			.setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);

		Handle = Device.GetHandle().createCommandPool(CmdPoolInfo);
		Device.GetRHI().GetDebugUtils().NameObject(Handle,
			Device.GetRHI().GetDebugUtils().MakeInternalName("CommandPool"));
	}

	auto FVulkanCommandBufferPool::Create() -> FVulkanCommandBuffer*
	{
		CheckVulkanRHIThread();
		FVulkanCommandBuffer* CmdBuffer = nullptr;

		if (!FreeCmdBuffers.empty())
		{
			CmdBuffer = FreeCmdBuffers.back();
			FreeCmdBuffers.pop_back();
			CmdBuffers.push_back(CmdBuffer);
			GVulkanMemoryBaselineTracker.RecordCommandBufferReuse();

			return CmdBuffer;
		}

		CmdBuffer = new FVulkanCommandBuffer(Device, this);
		CmdBuffers.push_back(CmdBuffer);
		GVulkanMemoryBaselineTracker.RecordCommandBufferAllocation();
		return CmdBuffer;
	}

	auto FVulkanCommandBufferPool::FreeUnusedCommandBuffers(FVulkanQueue* Queue) -> void
	{
		CheckVulkanRHIThread();
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
