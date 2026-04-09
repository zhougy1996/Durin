#include "VulkanContext.h"

#include "RHIResources.h"

#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanRenderPass.h"
#include "VulkanViewport.h"
#include "VulkanQueue.h"

namespace Doge::VulkanRHI
{
	FVulkanCommandListContext::FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice& InDevice, FVulkanQueue* InQueue)
		: RHI(InRHI)
		, Device(InDevice)
		, Queue(InQueue)
	{
		Pool = new FVulkanCommandPool(Device);
		Pool->CreatePool(Queue->GetFamilyIndex());
		CommandBuffer = Pool->Create(false);
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
		RenderPassManager.BeginRenderPass(*this, Device, CommandBuffer, RenderPassInfo, RenderPass, Framebuffer);
	}

	auto FVulkanCommandListContext::RHIEndRenderPass() -> void
	{
		Device.GetRenderPassManager().EndRenderPass(CommandBuffer);
	}

	auto FVulkanCommandListContext::RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void
	{
		// TODO: Now only use one command buffer repeatedly
		// Try use a new one each frame
		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);
		VulkanViewport->WaitForLastFrameCompletion();
		CommandBuffer->Begin();
		// End() is called before submit
	}

	auto FVulkanCommandListContext::RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
	{
		auto* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);

		FVulkanQueue* PresentQueue = Device.GetPresentQueue();
		VulkanViewport->Present(*this, *CommandBuffer, *PresentQueue, bLockToVsync);
	}

	auto FVulkanCommandListContext::RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void
	{
		PendingGfxPipelineState = static_cast<FVulkanGraphicsPipelineState*>(&GraphicsPipelineState);
		PendingGfxPipelineState->Bind(CommandBuffer->GetHandle());
	}

	auto FVulkanCommandListContext::RHIDrawPrimitive() -> void
	{
		PendingGfxPipelineState->PrepareForDraw(*this);
		CommandBuffer->GetHandle().draw(3, 1, 0, 0);
	}

	auto FVulkanCommandListContext::SubmitCmdBufferFromPresent(FVulkanSemaphore* SignalSemaphore) -> void
	{
		CommandBuffer->End();
		Queue->Submit(*CommandBuffer, SignalSemaphore);
		CommandBuffer = nullptr;
		PrepareForNewCommandBuffer();
	}

	auto FVulkanCommandListContext::RHISubmitCommandsHint() -> void
	{
		CommandBuffer->End();
		FVulkanQueue* GraphicsQueue = Device.GetGraphicsQueue();
		GraphicsQueue->Submit(*CommandBuffer, nullptr);
	}

	auto FVulkanCommandListContext::GetCommandBuffer() const -> FVulkanCommandBuffer*
	{
		return CommandBuffer;
	}

	auto FVulkanCommandListContext::PrepareForNewCommandBuffer() -> void
	{
		CommandBuffer = Pool->Create(false);
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
}
