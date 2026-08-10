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
	class FVulkanPendingGraphicsState;
	class FVulkanTexture;

	// Translates backend-neutral command-list operations into Vulkan command recording.
	class FVulkanCommandListContext : public IRHICommandContext
	{
	public:
		FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice& InDevice, FVulkanQueue* InQueue);

		~FVulkanCommandListContext() override;

		auto RHISetViewport(float MinX, float MinY, float MinZ, float MaxX, float MaxY, float MaxZ) -> void override;

		auto RHISetScissor(float MinX, float MinY, float Width, float Height) -> void override;

		auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void override;

		auto RHISubmitCommands() -> void override;

		auto RHIEndFrame() -> void override;

		auto RHIBeginRenderPass(const FRHIRenderPassInfo& RenderPassInfo, FName Name) -> void override;

		auto RHIEndRenderPass() -> void override;

		auto RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void override;

		auto RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void override;

		auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void override;

		auto RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* InVertexBuffer, uint32 Offset) -> void override;

		auto RHIBindIndexBuffer(FRHIBuffer* InIndexBuffer, uint32 Offset) -> void override;

		auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions) -> void override;

		auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions) -> void override;
		auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferCopyRegion> Regions) -> void override;
		auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override;
		auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void override;
		auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination,
			std::span<const FRHITextureCopyRegion> Regions) -> void override;

		auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset, std::span<const uint8> Data) -> void override;

		auto RHIInitializeTexture(FRHITexture* Texture) -> void override;

		auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, std::span<const uint8> SourceData) -> void override;

		auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, std::vector<uint8>& OutData) -> bool override;

		auto RHIAllocateDynamicUniformBuffer(const void* Data, uint32 Size) -> FRHIUniformBufferRange override;

		auto RHIAcquireBackBuffer(FRHITexture* BackBuffer) -> void override;

		auto RHIBlockUntilGPUIdle() -> void override;

		auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void override;

		auto RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void override;

		auto RHIDrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void override;

		auto GetCommandBuffer() -> FVulkanCommandBuffer*;

		auto RHISetShaderUniformBuffer(FRHIShader* InShader, uint32 SetIndex, uint32 BindIndex, FRHIBuffer* InUniformBuffer) -> void;

		auto AddWaitSemaphore(vk::PipelineStageFlags InWaitFlag, FVulkanSemaphore* InWaitSemaphore) -> void;

		auto AddWaitSemaphores(vk::PipelineStageFlags InWaitFlag, std::span<FVulkanSemaphore*> InWaitSemaphores) -> void;

		auto AddSignalSemaphore(FVulkanSemaphore* InSignalSemaphore) -> void;

		auto AddSignalSemaphores(std::span<FVulkanSemaphore*> InSignalSemaphores) -> void;

		auto GetQueue() const -> FVulkanQueue* { return Queue; }

		auto NotifyDeleted_Image(vk::Image Image) -> void;

		auto NotifyDeleted_GraphicsPipeline(
			FVulkanGraphicsPipelineState* PipelineState) -> void;

		// Submit and reset context
		auto Finalize() -> void;

	protected:
		auto PrepareNewCommandBuffer(FVulkanPayload& InPayload) -> void;

		auto GetPayload() -> FVulkanPayload&;

		FVulkanDynamicRHI* RHI = nullptr;

		FVulkanDevice& Device;

		FVulkanQueue* Queue = nullptr;

		FVulkanCommandBufferPool* Pool = nullptr;

		std::unique_ptr<FVulkanPendingGraphicsState> PendingGfxState;

		std::vector<FVulkanPayload*> Payloads;

		struct FPendingAttachmentState
		{
			FVulkanTexture* Texture = nullptr;
			FRHITextureSubresourceRange Range{};
			ERHIAccess FinalAccess = ERHIAccess::None;
		};

		std::vector<FPendingAttachmentState> PendingAttachmentStates;
	};
}
