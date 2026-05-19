#pragma once

#include "RHIShaderParameters.h"

namespace Durin
{
	struct FRHIRenderPassInfo;
	class FRHIGraphicsPipelineState;
	class FRHIViewport;
	class FRHITexture;

	class IRHICommandContext
	{
	public:
		virtual ~IRHICommandContext() = default;
		virtual auto RHIBeginFrame() -> void = 0;
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
		virtual auto RHIPushConstants(EShaderStageFlags StageFlags, uint32 Offset, uint32 Size, const void* Data) -> void = 0;
		virtual auto RHISetShaderParameters(FRHIShader* InShader, std::span<uint8> InParametersData) -> void = 0;
		virtual auto RHISetShaderParameters(FRHIShader* InShader, const std::span<FRHIShaderParameterResource>& InResourceParameters) -> void = 0;
		virtual auto RHIDrawIndexed(uint32 IndexCount, uint32 StartIndexLocation, int32 VertexOffset) -> void = 0;
	};
}