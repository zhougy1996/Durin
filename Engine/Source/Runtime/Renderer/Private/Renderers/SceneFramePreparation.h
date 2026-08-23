#pragma once

#include "IRendererModule.h"

namespace Durin
{
	class FSceneRenderer;
	class FRHICommandListImmediate;
	class FScene;
	struct FSceneView;
	struct FSceneRenderPlan;

	class FSceneFramePreparation final
	{
	public:
		auto Prepare_RenderThread(
			FSceneRenderer& Renderer,
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			FSceneView& View,
			const FSceneViewRenderOptions& Options,
			FSceneRenderPlan& Plan
		) const -> ERenderViewResult;
	};
} // namespace Durin
