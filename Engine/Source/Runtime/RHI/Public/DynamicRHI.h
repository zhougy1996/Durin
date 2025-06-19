#pragma once

#include "RHIGlobals.h"


class FRHIGraphicsPipelineState;
class FGraphicsPipelineStateInitializer;
class IRHICommandContext;
class FRHIViewport;
class FRHITexture;

class RHI_API IDynamicRHI
{
public:
	IDynamicRHI() = default;

	virtual ~IDynamicRHI() = default;

	virtual auto Init() -> void = 0;
	virtual auto Shutdown() -> void = 0;

	virtual auto RHICreateViewport(void* WindowHandle, uint32 SizeX, uint32 SizeY, bool bIsFullscreen, EPixelFormat PreferredPixelFormat) const -> TSharedPtr<FRHIViewport> = 0;
	virtual auto RHICreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TSharedPtr<FRHIGraphicsPipelineState> = 0;
	virtual auto RHIGetDefaultContext() -> IRHICommandContext* = 0;
	virtual auto RHIGetViewportBackBuffer(FRHIViewport* ViewportRHI) -> TSharedPtr<FRHITexture> = 0;
};


class RHI_API IDynamicRHIModule : public IModuleInterface
{
public:
	virtual auto CreateRHI() -> IDynamicRHI* = 0;
};

template<typename TRHI>
FORCEINLINE auto CastDynamicRHI(IDynamicRHI* DynamicRHI) -> TRHI*
{
	return static_cast<TRHI*>(DynamicRHI);
}

template<typename TRHI>
FORCEINLINE auto GetDynamicRHI() -> TRHI*
{
	return CastDynamicRHI<TRHI>(GDynamicRHI);
}
