#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;

	using FSceneColorTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FPostProcessTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);
	using FHDRSceneColorCaptureSink = void (*)(
		FRHICommandListImmediate& CommandList,
		FRHITexture* SceneColor,
		FRHITexture* PostProcessInput);

	// Development seam receiving each explicitly requested Scene Color GPU interval.
	RENDERER_API auto SetSceneColorTimingQuerySink(
		FSceneColorTimingQuerySink Sink) -> void;

	// Development seam receiving the copy or FXAA display-output GPU interval.
	RENDERER_API auto SetPostProcessTimingQuerySink(
		FPostProcessTimingQuerySink Sink) -> void;

	// Development seam receiving scene-linear color after optional contact
	// composition and before display mapping.
	RENDERER_API auto SetHDRSceneColorCaptureSink(
		FHDRSceneColorCaptureSink Sink) -> void;
}
