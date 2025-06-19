#pragma once

#include "RHIConstants.h"

class FRHICommandListImmediate;

class RHI_API FRHITexture{};

class RHI_API FRHIViewport
{
public:
	virtual auto Tick(float DeltaTime) -> void {};
	virtual auto GetBackBuffer(FRHICommandListImmediate& RHICmdList) -> TSharedPtr<FRHITexture> = 0;
	virtual auto WaitForLastFrameCompletion() -> void = 0;
};

struct RHI_API FRHIRenderTargetsInfo
{
	FRHITexture* ColorRenderTargets[MAX_SIMULTANEOUS_RENDER_TARGETS];
	int32 NumColorRenderTargets;
	bool bClearColor;
};

struct RHI_API FRHIRenderPassInfo
{
	FRHITexture* ColorRenderTargets[MAX_SIMULTANEOUS_RENDER_TARGETS];
};

class RHI_API FGraphicsPipelineStateInitializer{

};

struct RHI_API FRHIVertexBuffer
{
	FRHITexture* ColorRenderTargets[MAX_SIMULTANEOUS_RENDER_TARGETS];
};