#pragma once

#include "RHIShaderParameters.h"
#include "RHIResources.h"

namespace Durin
{
	struct FRHIRenderPassInfo;
	class FRHIGraphicsPipelineState;
	class FRHIViewport;
	class FRHITexture;

	// Defines the backend command-recording operations consumed by RHI command lists.
	class IRHICommandContext
	{
	public:
		virtual ~IRHICommandContext() = default;
		virtual auto RHIBeginFrame(const FRHIBeginFrameArgs& Args) -> void = 0;
		virtual auto RHISubmitCommands() -> void = 0;
		virtual auto RHIEndFrame() -> void = 0;
		virtual auto RHIBeginRenderPass(const FRHIRenderPassInfo& InInfo, FName InName) -> void = 0;
		virtual auto RHIEndRenderPass() -> void = 0;
		virtual auto RHIBeginDrawingViewport(FRHIViewport* InViewport, FRHITexture* InRenderTargetRHI) -> void = 0;
		virtual auto RHIEndDrawingViewport(FRHIViewport* InViewport, bool bInPresent, bool bInLockToVsync) -> void = 0;
		virtual auto RHISetViewport(float InMinX, float InMinY, float InMinZ, float InMaxX, float InMaxY, float InMaxZ) -> void = 0;
		virtual auto RHISetScissor(float InMinX, float InMinY, float InWidth, float InHeight) -> void = 0;
		virtual auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& InGraphicsPipelineState) -> void = 0;
		virtual auto RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void = 0;
		virtual auto RHIBindIndexBuffer(FRHIBuffer* IndexBuffer, uint32 Offset) -> void = 0;
		virtual auto RHITransitionBuffers(std::span<const FRHIBufferTransition> Transitions) -> void = 0;
		virtual auto RHITransitionTextures(std::span<const FRHITextureTransition> Transitions) -> void = 0;
		virtual auto RHICopyBuffer(FRHIBuffer* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferCopyRegion> Regions) -> void = 0;
		virtual auto RHICopyBufferToTexture(FRHIBuffer* Source, FRHITexture* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void = 0;
		virtual auto RHICopyTextureToBuffer(FRHITexture* Source, FRHIBuffer* Destination,
			std::span<const FRHIBufferTextureCopyRegion> Regions) -> void = 0;
		virtual auto RHICopyTexture(FRHITexture* Source, FRHITexture* Destination,
			std::span<const FRHITextureCopyRegion> Regions) -> void = 0;
		virtual auto RHIWriteBuffer(FRHIBuffer* Buffer, uint32 Offset, std::span<const uint8> Data) -> void = 0;
		virtual auto RHIInitializeTexture(FRHITexture* Texture) -> void = 0;
		virtual auto RHIUpdateTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, std::span<const uint8> SourceData) -> void = 0;
		virtual auto RHIReadTexture2D(FRHITexture* Texture, uint32 MipIndex, uint32 ArraySlice, std::vector<uint8>& OutData) -> bool = 0;
		virtual auto RHIAllocateDynamicUniformBuffer(const void* Data, uint32 Size) -> FRHIUniformBufferRange = 0;
		virtual auto RHIAcquireBackBuffer(FRHITexture* BackBuffer) -> void = 0;
		virtual auto RHIBlockUntilGPUIdle() -> void = 0;
		virtual auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void = 0;
		virtual auto RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void = 0;
		virtual auto RHIDraw(const FRHIDrawArguments& Arguments) -> void = 0;
		virtual auto RHIDrawIndexed(const FRHIDrawIndexedArguments& Arguments) -> void = 0;
	};
}
