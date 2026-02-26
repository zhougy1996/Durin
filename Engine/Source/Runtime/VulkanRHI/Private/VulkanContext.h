#pragma once

#include "RHIContext.h"

namespace Doge::VulkanRHI
{
	class FVulkanDynamicRHI;
	class FVulkanDevice;
	class FVulkanQueue;
	class FVulkanGraphicsPipelineState;
	class FVulkanCommandBufferManager;
	class FVulkanCommandBuffer;

	class FVulkanCommandListContext : public IRHICommandContext
	{
	public:
		FVulkanCommandListContext(FVulkanDynamicRHI* RHI, FVulkanDevice& Device, FVulkanQueue* Queue);

		auto RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void override;

		auto RHIBeginFrame() -> void override;

		auto RHIEndFrame() -> void override;

		auto RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void override;

		auto RHIEndRenderPass() -> void override;

		auto RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void override;

		auto RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void override;

		auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void override;

		auto RHIDrawPrimitive() -> void override;

		auto RHISubmitCommandsHint() -> void override;

		auto GetCommandBufferManager() const -> FVulkanCommandBufferManager* { return CommandBufferManager_; }

		auto GetCommandBuffer() const -> FVulkanCommandBuffer*;

		auto GetQueue() const -> FVulkanQueue* { return Queue_; }

	protected:
		FVulkanDynamicRHI* RHI_;

		FVulkanDevice& Device_;

		FVulkanQueue* Queue_;

		FVulkanCommandBufferManager* CommandBufferManager_;

		FVulkanGraphicsPipelineState* PendingGfxPipelineState_ = nullptr;
	};
}
