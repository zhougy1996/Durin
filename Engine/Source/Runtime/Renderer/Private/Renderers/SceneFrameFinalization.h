#pragma once

#include "IRendererModule.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/PostProcessRenderer.h"

namespace Durin
{
	class FSceneRenderer;
	class FRHICommandListImmediate;
	class FRHITexture;
	struct FSceneRenderPlan;
	struct FSceneView;

	class FSceneFrameFinalization final
	{
	public:
		auto Finalize_RenderThread(
			FSceneRenderer& Renderer,
			FRHICommandListImmediate& CommandList,
			FSceneRenderPlan& Plan,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FPostProcessRenderer::FSceneTargets* SceneTargets,
			FGBufferRenderer::FTargets* GBufferTargets,
			FRHITexture* SceneColor,
			FRHITexture* AmbientOcclusionDebugOutput
		) const -> ERenderViewResult;
	};
} // namespace Durin
