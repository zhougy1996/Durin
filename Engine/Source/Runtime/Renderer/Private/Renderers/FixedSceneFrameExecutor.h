#pragma once

#include "IRendererModule.h"

namespace Durin
{
	class FSceneRenderer;
	class FRHICommandListImmediate;
	class FRHITexture;
	class FScene;
	struct FSceneView;

	class FFixedSceneFrameExecutor final
	{
	public:
		auto Execute_RenderThread(
			FSceneRenderer& Renderer,
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics
		) const -> ERenderViewResult;
	};
} // namespace Durin
