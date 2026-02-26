#pragma once

#include "RHIContext.h"

namespace Doge::VulkanRHI
{
	class FVulkanDynamicRHI;
	class FVulkanDevice;
	class FVulkanQueue;
	class FVulkanGraphicsPipelineState;
	class FVulkanCommandBufferManager;

	class FVulkanCommandListContext : public IRHICommandContext
	{
	public:
		FVulkanCommandListContext(FVulkanDynamicRHI* RHI, FVulkanDevice& Device, FVulkanQueue* Queue);

		virtual auto RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void;

		virtual auto RHIBeginFrame() -> void override;

		virtual auto RHIEndFrame() -> void override;

		virtual auto RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void override;

		virtual auto RHIEndRenderPass() -> void override;

		virtual auto RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void override;

		virtual auto RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void override;

		virtual auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void override;

		virtual auto RHIDrawPrimitive() -> void override;

		virtual auto RHITestCommandRecord() -> void override;

		virtual auto RHISubmitCommandsHint() -> void override;

		auto GetCommandBufferManager() const -> FVulkanCommandBufferManager* { return CommandBufferManager_; }

		auto GetQueue() const -> FVulkanQueue* { return Queue_; }

	protected:
		FVulkanDynamicRHI* RHI_;

		FVulkanDevice& Device_;

		FVulkanQueue* Queue_;

		FVulkanCommandBufferManager* CommandBufferManager_;

		FVulkanGraphicsPipelineState* PendingGfxPipelineState_ = nullptr;
	};
}
