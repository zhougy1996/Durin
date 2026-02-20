#pragma once

struct FRHIRenderPassInfo;
class FRHIGraphicsPipelineState;
class FRHIViewport;
class FRHITexture;

class RHI_API IRHICommandContext
{
public:
	virtual auto RHIBeginFrame() -> void = 0;
	virtual auto RHIEndFrame() -> void = 0;
	virtual auto RHIBeginRenderPass(const FRHIRenderPassInfo& Info, FName Name) -> void = 0;
	virtual auto RHIEndRenderPass() -> void = 0;
	virtual auto RHIBeginDrawingViewport(FRHIViewport* Viewport, FRHITexture* RenderTargetRHI) -> void = 0;
	virtual auto RHIEndDrawingViewport(FRHIViewport* Viewport, bool bPresent, bool bLockToVsync) -> void = 0;
	virtual auto RHISetGraphicsPipelineState(FRHIGraphicsPipelineState& GraphicsPipelineState) -> void = 0;
	virtual auto RHISubmitCommandsHint() -> void = 0;
	virtual auto RHIDrawPrimitive() -> void = 0;
	virtual auto RHITestCommandRecord() -> void = 0;
};