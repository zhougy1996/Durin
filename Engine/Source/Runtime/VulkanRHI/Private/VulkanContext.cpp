#include "VulkanContext.h"

#include "RHIResources.h"

#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanRenderPass.h"
#include "VulkanViewport.h"
#include "VulkanQueue.h"
#include "FVulkanSubmission.h"
#include "VulkanBuffer.h"
#include "VulkanRHIPrivate.h"

namespace Doge::VulkanRHI
{
	FVulkanCommandListContext::FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice& InDevice, FVulkanQueue* InQueue)
		: RHI(InRHI)
		, Device(InDevice)
		, Queue(InQueue)
	{
		Pool = new FVulkanCommandBufferPool(Device);
		Pool->CreatePool(Queue->GetFamilyIndex());
	}

	FVulkanCommandListContext::~FVulkanCommandListContext()
	{
		delete Pool;
	}

	auto FVulkanCommandListContext::RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
	{
		PendingGfxPipelineState->SetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
	}

	auto FVulkanCommandListContext::RHIBeginFrame() -> void
	{
	}

	auto FVulkanCommandListContext::RHIEndFrame() -> void
	{
		FVulkanFrame& Frame = Device.GetCurrentFrame();
		FVulkanPayload& Payload = GetPayload();
		Payload.Fence = Frame.GetFrameFence();
		Finalize();

		Pool->FreeUnusedCommandBuffers(Queue);
	}

	auto FVulkanCommandListContext::RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void
	{
		const auto* VulkanRT = static_cast<FVulkanTexture*>(RenderPassInfo.ColorRenderTargets[0]);

		FVulkanRenderPassManager& RenderPassManager = Device.GetRenderPassManager();
		FVulkanRenderPass* RenderPass = Device.GetRenderPassManager().GetOrCreateRenderPass(Name, VulkanRT->Format);

		FRHIRenderTargetsInfo RTInfo{};
		RTInfo.NumColorRenderTargets = 1;
		RTInfo.ColorRenderTargets[0] = RenderPassInfo.ColorRenderTargets[0];
		FVulkanFramebuffer* Framebuffer = RenderPassManager.GetOrCreateFrameBuffer(RTInfo);
		RenderPassManager.BeginRenderPass(*this, Device, GetCommandBuffer(), RenderPassInfo, RenderPass, Framebuffer);
	}

	auto FVulkanCommandListContext::RHIEndRenderPass() -> void
	{
		Device.GetRenderPassManager().EndRenderPass(GetCommandBuffer());
	}

	auto FVulkanCommandListContext::RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void
	{
	}

	auto FVulkanCommandListContext::RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);

		FVulkanQueue* PresentQueue = Device.GetPresentQueue();
		VulkanViewport->Present(*this, *GetCommandBuffer(), *PresentQueue, bLockToVsync);
	}

	auto FVulkanCommandListContext::RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void
	{
		PendingGfxPipelineState = static_cast<FVulkanGraphicsPipelineState*>(&GraphicsPipelineState);
		PendingGfxPipelineState->Bind(GetCommandBuffer()->GetHandle());
	}

	auto FVulkanCommandListContext::RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* InVertexBuffer, uint32 Offset) -> void
	{
		vk::Buffer BufferHandle = static_cast<FVulkanBuffer*>(InVertexBuffer)->GetHandle();
		GetCommandBuffer()->GetHandle().bindVertexBuffers(StreamIndex, BufferHandle, {Offset});
	}

	static constexpr auto DeduceIndexType(uint32 Stride) -> vk::IndexType
	{
		if (Stride == 2) { return vk::IndexType::eUint16; }
		if (Stride == 4) { return vk::IndexType::eUint32; }
		checkf(false, "Unsupported index buffer stride: {}", Stride);
	}

	auto FVulkanCommandListContext::RHIBindIndexBuffer(FRHIBuffer* InIndexBuffer, uint32 Offset) -> void
	{
		const FVulkanBuffer* IndexBuffer = static_cast<FVulkanBuffer*>(InIndexBuffer);
		GetCommandBuffer()->GetHandle().bindIndexBuffer(IndexBuffer->GetHandle(), Offset, DeduceIndexType(IndexBuffer->GetStride()));
	}

	auto FVulkanCommandListContext::RHIDrawPrimitive() -> void
	{
		PendingGfxPipelineState->PrepareForDraw(*this);
		GetCommandBuffer()->GetHandle().drawIndexed(6, 1, 0, 0, 0);
	}

	auto FVulkanCommandListContext::GetCommandBuffer() -> FVulkanCommandBuffer*
	{
		FVulkanPayload& Payload = GetPayload();
		if (Payload.CommandBuffers.empty())
		{
			PrepareNewCommandBuffer(Payload);
		}
		return Payload.CommandBuffers.back();
	}

	auto FVulkanCommandListContext::AddWaitSemaphore(vk::PipelineStageFlags InWaitFlag, FVulkanSemaphore* InWaitSemaphore) -> void
	{
		AddWaitSemaphores(InWaitFlag, std::span(&InWaitSemaphore, 1));
	}

	auto FVulkanCommandListContext::AddWaitSemaphores(vk::PipelineStageFlags InWaitFlag, std::span<FVulkanSemaphore*> InWaitSemaphores) -> void
	{
		auto& Payload = GetPayload();
		Payload.WaitSemaphores.insert(Payload.WaitSemaphores.end(), InWaitSemaphores.begin(), InWaitSemaphores.end());
		Payload.WaitFlags.insert(Payload.WaitFlags.end(), InWaitSemaphores.size(), InWaitFlag);
	}

	auto FVulkanCommandListContext::AddSignalSemaphore(FVulkanSemaphore* InSignalSemaphore) -> void
	{
 		AddSignalSemaphores(std::span(&InSignalSemaphore, 1));
	}

	auto FVulkanCommandListContext::AddSignalSemaphores(std::span<FVulkanSemaphore*> InSignalSemaphores) -> void
	{
		auto& Payload = GetPayload();
		Payload.SignalSemaphores.insert(Payload.SignalSemaphores.end(), InSignalSemaphores.begin(), InSignalSemaphores.end());
	}

	auto FVulkanCommandListContext::Finalize() -> void
	{
		GetCommandBuffer()->End();
		Queue->SubmitPayloads(Payloads);
		Payloads.clear();
	}

	auto FVulkanCommandListContext::PrepareNewCommandBuffer(FVulkanPayload& InPayload) -> void
	{
		check(InPayload.CommandBuffers.empty());
		FVulkanCommandBuffer* NewCmdBuffer = Pool->Create();
		InPayload.CommandBuffers.push_back(NewCmdBuffer);
		NewCmdBuffer->Begin();
	}

	auto FVulkanCommandListContext::GetPayload() -> FVulkanPayload&
	{
		// Currently only support one payload per submit.
		if (Payloads.empty())
		{
			Payloads.push_back(new FVulkanPayload(*Queue));
		}
		return *Payloads.back();
	}

	auto FVulkanDynamicRHI::RHIGetDefaultContext() -> IRHICommandContext*
	{
		return Device->GetImmediateContext();
	}

	auto FVulkanDynamicRHI::RHIGetCommandContext(ERHIPipeline Pipeline) const -> IRHICommandContext*
	{
		if (Pipeline != ERHIPipeline::Graphics)
		{
			return nullptr;
		}

		return Device->GetImmediateContext();
	}
} // namespace Doge::VulkanRHI
