#pragma once

namespace Doge
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
		virtual auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& InGraphicsPipelineState) -> void = 0;
		virtual auto RHIBindVertexBuffer(uint32 StreamIndex, FRHIBuffer* VertexBuffer, uint32 Offset) -> void = 0;
		virtual auto RHIDrawPrimitive() -> void = 0;
		virtual auto RHISetViewport(float InMinX, float InMinY, float InMinZ, float InMaxX, float InMaxY, float InMaxZ) -> void = 0;
	};
}