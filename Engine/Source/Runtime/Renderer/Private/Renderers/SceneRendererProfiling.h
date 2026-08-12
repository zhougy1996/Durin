#pragma once

#include "RendererAPI.h"
#include "RHIResources.h"

namespace Durin
{
	using FSceneColorTimingQuerySink = void (*)(
		const FGPUTimingQueryRHIRef& Query);

	// Development seam receiving each explicitly requested Scene Color GPU interval.
	RENDERER_API auto SetSceneColorTimingQuerySink(
		FSceneColorTimingQuerySink Sink) -> void;
}
