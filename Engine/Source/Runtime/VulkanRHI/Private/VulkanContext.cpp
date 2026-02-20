#include "VulkanContext.h"

#include "RHIResources.h"

#include "VulkanDynamicRHI.h"
#include "VulkanDevice.h"
#include "VulkanCommandBuffer.h"
#include "VulkanPipeline.h"
#include "VulkanRenderPass.h"
#include "VulkanViewport.h"
#include "VulkanQueue.h"

FVulkanCommandListContext::FVulkanCommandListContext(FVulkanDynamicRHI* RHI, FVulkanDevice& Device, FVulkanQueue* Queue)
	: RHI_(RHI)
	, Device_(Device)
	, Queue_(Queue)
{
	CommandBufferManager_ = new FVulkanCommandBufferManager(Device_, *this);
}

auto FVulkanCommandListContext::RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void
{
	PendingGfxPipelineState_->SetViewport(MinX, MinY, MinZ, MaxX, MaxY, MaxZ);
}

auto FVulkanCommandListContext::RHIBeginFrame() -> void
{
}

auto FVulkanCommandListContext::RHIEndFrame() -> void
{
	CommandBufferManager_->FreeUnusedCommandBuffers();
}

auto FVulkanCommandListContext::RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void
{
	FVulkanCommandBuffer* CmdBuffer = CommandBufferManager_->GetActiveCommandBuffer();

	FVulkanTexture* VulkanRT = static_cast<FVulkanTexture*>(RenderPassInfo.ColorRenderTargets[0]);

	FVulkanRenderPassManager& RenderPassManager = Device_.GetRenderPassManager();
	FVulkanRenderPass* RenderPass = Device_.GetRenderPassManager().GetOrCreateRenderPass(Name, VulkanRT->Format_);

	FRHIRenderTargetsInfo RTInfo;
	RTInfo.NumColorRenderTargets = 1;
	RTInfo.ColorRenderTargets[0] = RenderPassInfo.ColorRenderTargets[0];
	FVulkanFramebuffer* Framebuffer = RenderPassManager.GetOrCreateFrameBuffer(RTInfo);
	RenderPassManager.BeginRenderPass(*this, Device_, CmdBuffer, RenderPassInfo, RenderPass, Framebuffer);
}

auto FVulkanCommandListContext::RHIEndRenderPass() -> void
{
	FVulkanCommandBuffer* CmdBuffer = CommandBufferManager_->GetActiveCommandBuffer();
	Device_.GetRenderPassManager().EndRenderPass(CmdBuffer);
}

auto FVulkanCommandListContext::RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void
{
	// TODO: Now only use one command buffer repeatly
	// Try use a new one each frame
	FVulkanViewport* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);
	VulkanViewport->WaitForLastFrameCompletion();
	FVulkanCommandBuffer* CmdBuffer = CommandBufferManager_->GetActiveCommandBuffer();
	CmdBuffer->Begin();
	// End() is called before submit
}

auto FVulkanCommandListContext::RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void
{
	FVulkanViewport* VulkanViewport = static_cast<FVulkanViewport*>(Viewport);

	FVulkanQueue* PresentQueue = Device_.GetPresentQueue();
	VulkanViewport->Present(*this, *CommandBufferManager_->GetActiveCommandBuffer(), *PresentQueue, bLockToVsync);
}

auto FVulkanCommandListContext::RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void
{
	PendingGfxPipelineState_ = static_cast<FVulkanGraphicsPipelineState*>(&GraphicsPipelineState);
	FVulkanCommandBuffer* CmdBuffer = CommandBufferManager_->GetActiveCommandBuffer();

	PendingGfxPipelineState_->Bind(CmdBuffer->GetHandle());
}

auto FVulkanCommandListContext::RHIDrawPrimitive() -> void
{
	FVulkanCommandBuffer* CmdBuffer = CommandBufferManager_->GetActiveCommandBuffer();
	CmdBuffer->GetHandle().draw(3, 1, 0, 0);
}

auto FVulkanCommandListContext::RHITestCommandRecord() -> void
{
	FVulkanCommandBuffer* CmdBuffer = CommandBufferManager_->GetActiveCommandBuffer();
	vk::Viewport viewport;
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = 800.0f;
	viewport.height = 600.0f;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	CmdBuffer->GetHandle().setViewport(0, viewport);


	vk::Rect2D scissor;
	scissor.setOffset({static_cast<int32>(0), static_cast<int32>(0)});
	scissor.setExtent({800, 600});
	CmdBuffer->GetHandle().setScissor(0, scissor);
}

auto FVulkanCommandListContext::RHISubmitCommandsHint() -> void
{
	FVulkanCommandBuffer* CmdBuffer = CommandBufferManager_->GetActiveCommandBuffer();
	CmdBuffer->End();
	FVulkanQueue* GraphicsQueue = Device_.GetGraphicsQueue();
	GraphicsQueue->Submit(*CmdBuffer, nullptr);
}

auto FVulkanDynamicRHI::RHIGetDefaultContext() -> IRHICommandContext*
{
	return Device_->GetImmediateContext();
}

auto FVulkanDynamicRHI::RHIGetCommandContext(ERHIPipeline Pipeline) -> IRHICommandContext*
{
	if (Pipeline != ERHIPipeline::eGraphics)
	{
		return nullptr;
	}

	return Device_->GetImmediateContext();
}
