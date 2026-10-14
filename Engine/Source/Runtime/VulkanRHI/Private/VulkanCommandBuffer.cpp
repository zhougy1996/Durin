#include "VulkanCommandBuffer.h"


#include "VulkanDevice.h"
#include "VulkanDynamicRHI.h"
#include "VulkanDiagnostics.h"
#include "VulkanMemory.h"
#include "VulkanRenderPass.h"
#include "VulkanFramebuffer.h"
#include "VulkanRHIPrivate.h"
#include "VulkanProfiling.h"

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
		RecordedPipelines = {};
		RecordedVertexBuffers.clear();
		RecordedIndexBuffer.reset();
		RecordedViewport.reset();
		RecordedScissor.reset();
		RecordedDepthBias.reset();
		for (auto& Binding : RecordedDescriptors)
		{
			Binding.Layout = nullptr;
			Binding.Sets.clear();
			Binding.DynamicOffsets.clear();
		}
		State = EState::IsInsideBegin;
	}

	auto FVulkanCommandBuffer::BindPipeline(vk::PipelineBindPoint BindPoint, vk::Pipeline Pipeline) -> void
	{
		DURIN_VULKAN_PROFILE_DETAIL("Vulkan.Bind.Pipeline");
		CheckVulkanRHIThread();
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		check(BindPoint == vk::PipelineBindPoint::eGraphics || BindPoint == vk::PipelineBindPoint::eCompute);
		auto& Recorded = RecordedPipelines[BindPoint == vk::PipelineBindPoint::eGraphics ? 0 : 1];
		if (Recorded == Pipeline) return;
		{
			DURIN_VULKAN_PROFILE_DETAIL("Vulkan.Bind.PipelineNative");
			Handle.bindPipeline(BindPoint, Pipeline);
		}
		Recorded = Pipeline;
	}

	auto FVulkanCommandBuffer::BindVertexBuffer(uint32 Stream, vk::Buffer Buffer, vk::DeviceSize Offset) -> void
	{
		DURIN_VULKAN_PROFILE_DETAIL("Vulkan.Bind.VertexBuffer");
		CheckVulkanRHIThread();
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		check(Stream < Device.GetGpuProperties().limits.maxVertexInputBindings);
		if (RecordedVertexBuffers.size() <= Stream) RecordedVertexBuffers.resize(Stream + 1);
		auto& Recorded = RecordedVertexBuffers[Stream];
		const auto Binding = std::make_tuple(Buffer, Offset);
		if (Recorded == Binding) return;
		{
			DURIN_VULKAN_PROFILE_DETAIL("Vulkan.Bind.VertexBufferNative");
			Handle.bindVertexBuffers(Stream, Buffer, {Offset});
		}
		Recorded = Binding;
	}

	auto FVulkanCommandBuffer::BindIndexBuffer(vk::Buffer Buffer, vk::DeviceSize Offset, vk::IndexType Type) -> void
	{
		DURIN_VULKAN_PROFILE_DETAIL("Vulkan.Bind.IndexBuffer");
		CheckVulkanRHIThread();
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		const auto Binding = std::make_tuple(Buffer, Offset, Type);
		if (RecordedIndexBuffer && *RecordedIndexBuffer == Binding) return;
		{
			DURIN_VULKAN_PROFILE_DETAIL("Vulkan.Bind.IndexBufferNative");
			Handle.bindIndexBuffer(Buffer, Offset, Type);
		}
		RecordedIndexBuffer = Binding;
	}

	auto FVulkanCommandBuffer::SetGraphicsDynamicState(const vk::Viewport& Viewport,
		const vk::Rect2D& Scissor, float DepthBiasConstant, float DepthBiasClamp,
		float DepthBiasSlope) -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		if (!RecordedViewport || *RecordedViewport != Viewport)
		{
			Handle.setViewport(0, Viewport);
			RecordedViewport = Viewport;
		}
		if (!RecordedScissor || *RecordedScissor != Scissor)
		{
			Handle.setScissor(0, Scissor);
			RecordedScissor = Scissor;
		}
		const std::array DepthBias{DepthBiasConstant, DepthBiasClamp, DepthBiasSlope};
		if (!RecordedDepthBias || *RecordedDepthBias != DepthBias)
		{
			Handle.setDepthBias(DepthBiasConstant, DepthBiasClamp, DepthBiasSlope);
			RecordedDepthBias = DepthBias;
		}
	}

	auto FVulkanCommandBuffer::BindDescriptorSets(vk::PipelineBindPoint BindPoint,
		vk::PipelineLayout Layout, std::span<const vk::DescriptorSet> Sets,
		std::span<const uint32> DynamicOffsets) -> void
	{
		CheckVulkanRHIThread();
		check(State == EState::IsInsideBegin || State == EState::IsInsideRenderPass);
		check(BindPoint == vk::PipelineBindPoint::eGraphics || BindPoint == vk::PipelineBindPoint::eCompute);
		auto& Binding = RecordedDescriptors[BindPoint == vk::PipelineBindPoint::eGraphics ? 0 : 1];
		if (Binding.Layout == Layout && std::ranges::equal(Binding.Sets, Sets)
			&& std::ranges::equal(Binding.DynamicOffsets, DynamicOffsets)) return;
		Handle.bindDescriptorSets(BindPoint, Layout, 0, Sets, DynamicOffsets);
		Binding.Layout = Layout;
		Binding.Sets.assign(Sets.begin(), Sets.end());
		Binding.DynamicOffsets.assign(DynamicOffsets.begin(), DynamicOffsets.end());
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
		Device.GetRHI().GetDebugUtils().NameObjectLazy(Handle,
			[&] { return Device.GetRHI().GetDebugUtils().MakeInternalName("CommandBuffer"); });

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
			bRenderPassDebugLabelOpen =
				Device.GetRHI().GetDebugUtils().BeginNameLabel(Handle, DebugName);
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
		Device.GetRHI().GetDebugUtils().NameObjectLazy(Handle,
			[&] { return Device.GetRHI().GetDebugUtils().MakeInternalName("CommandPool"); });
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
