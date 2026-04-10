#pragma once

#include "RHIContext.h"
#include "VulkanMemory.h"

namespace Doge::VulkanRHI
{
	class FVulkanDynamicRHI;
	class FVulkanDevice;
	class FVulkanQueue;
	class FVulkanGraphicsPipelineState;
	class FVulkanCommandBufferManager;
	class FVulkanCommandBuffer;
	class FVulkanCommandBufferPool;
	class FVulkanSemaphore;

	class FVulkanCommandListContext : public IRHICommandContext
	{
	public:
		FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice& InDevice, FVulkanQueue* InQueue);

		~FVulkanCommandListContext() override;

		auto RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void override;

		auto RHIBeginFrame() -> void override;

		auto RHIEndFrame() -> void override;

		auto RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void override;

		auto RHIEndRenderPass() -> void override;

		auto RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void override;

		auto RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void override;

		auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void override;

		auto RHIDrawPrimitive() -> void override;

		auto SubmitCmdBufferFromPresent(FVulkanSemaphore* SignalSemaphore) -> void;

		auto RHISubmitCommandsHint() -> void override;

		auto GetCommandBuffer() const -> FVulkanCommandBuffer*;

		auto AddWaitSemaphore(FVulkanSemaphore* Semaphore) -> void;

		auto GetQueue() const -> FVulkanQueue* { return Queue; }

	protected:
		auto PrepareForNewCommandBuffer() -> void;

		FVulkanDynamicRHI* RHI = nullptr;

		FVulkanDevice& Device;

		FVulkanQueue* Queue = nullptr;

		FVulkanCommandBufferPool* Pool = nullptr;

		FVulkanCommandBuffer* CommandBuffer = nullptr;

		std::vector<FVulkanSemaphore*> WaitSemaphores;

		FVulkanGraphicsPipelineState* PendingGfxPipelineState = nullptr;
	};
}
