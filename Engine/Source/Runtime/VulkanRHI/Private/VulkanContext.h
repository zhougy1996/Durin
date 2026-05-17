#pragma once

#include "RHIContext.h"
#include "VulkanMemory.h"

namespace Durin::VulkanRHI
{
	class FVulkanDynamicRHI;
	class FVulkanDevice;
	class FVulkanQueue;
	class FVulkanGraphicsPipelineState;
	class FVulkanCommandBufferManager;
	class FVulkanCommandBuffer;
	class FVulkanCommandBufferPool;
	class FVulkanSemaphore;
	class FVulkanPayload;

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

		auto RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* InVertexBuffer, uint32 Offset) -> void override;

		auto RHIBindIndexBuffer(FRHIBuffer* InIndexBuffer, uint32 Offset) -> void override;

		auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void override;

		auto RHISetShaderParameters(FRHIShader* InShader, std::span<uint8> InParametersData) -> void override;

		auto RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void override;

		auto RHIDrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void override;

		auto GetCommandBuffer() -> FVulkanCommandBuffer*;

		auto RHISetShaderUniformBuffer(FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FRHIBuffer* InUniformBuffer) -> void;

		auto AddWaitSemaphore(vk::PipelineStageFlags InWaitFlag, FVulkanSemaphore* InWaitSemaphore) -> void;

		auto AddWaitSemaphores(vk::PipelineStageFlags InWaitFlag, std::span<FVulkanSemaphore*> InWaitSemaphores) -> void;

		auto AddSignalSemaphore(FVulkanSemaphore* InSignalSemaphore) -> void;

		auto AddSignalSemaphores(std::span<FVulkanSemaphore*> InSignalSemaphores) -> void;

		auto GetQueue() const -> FVulkanQueue* { return Queue; }

		// Submit and reset context
		auto Finalize() -> void;

	protected:
		auto PrepareNewCommandBuffer(FVulkanPayload& InPayload) -> void;

		auto GetPayload() -> FVulkanPayload&;

		FVulkanDynamicRHI* RHI = nullptr;

		FVulkanDevice& Device;

		FVulkanQueue* Queue = nullptr;

		FVulkanCommandBufferPool* Pool = nullptr;

		FVulkanGraphicsPipelineState* PendingGfxPipelineState = nullptr;

		std::vector<FVulkanPayload*> Payloads;
	};
}
