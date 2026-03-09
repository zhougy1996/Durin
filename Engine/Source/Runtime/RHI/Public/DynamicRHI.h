#pragma once

#include "PixelFormat.h"

namespace Doge
{
	class FGraphicsPipelineStateInitializer;
	class FRHIGraphicsPipelineState;
	class IRHICommandContext;
	class FRHITexture;
	class FRHIViewport;

	class RHI_API IDynamicRHI
	{
	public:
		IDynamicRHI() = default;

		virtual ~IDynamicRHI() = default;

		virtual auto Init() -> void = 0;
		virtual auto Shutdown() -> void = 0;

		virtual auto RHICreateViewport(void* InWindowHandle, uint32 InSizeX, uint32 InSizeY, bool bInIsFullscreen, EPixelFormat InPreferredPixelFormat) const -> TSharedPtr<FRHIViewport> = 0;
		virtual auto RHICreateGraphicsPipelineState(const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState> = 0;
		virtual auto RHIGetDefaultContext() -> IRHICommandContext* = 0;
		virtual auto RHIGetViewportBackBuffer(FRHIViewport* InViewportRHI) -> TRefCountPtr<FRHITexture> = 0;

		virtual auto RHIBlockUntilGPUIdle() -> void = 0;
	};

	extern RHI_API IDynamicRHI* GDynamicRHI;

	class RHI_API IDynamicRHIModule : public IModuleInterface
	{
	public:
		virtual auto CreateRHI() -> IDynamicRHI* = 0;
	};

	template<typename TRHI>
	FORCEINLINE auto CastDynamicRHI(IDynamicRHI* InDynamicRHI) -> TRHI*
	{
		return static_cast<TRHI*>(InDynamicRHI);
	}

	template<typename TRHI>
	FORCEINLINE auto GetDynamicRHI() -> TRHI*
	{
		return CastDynamicRHI<TRHI>(GDynamicRHI);
	}
}