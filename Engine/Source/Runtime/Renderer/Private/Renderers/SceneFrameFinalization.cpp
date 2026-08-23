#include "Renderers/SceneFrameFinalization.h"

#include "Renderers/SceneRenderer.h"

namespace Durin
{
	auto FSceneFrameFinalization::Finalize_RenderThread(
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
	) const -> ERenderViewResult
	{
		return Renderer.RenderPostProcess_RenderThread(
			CommandList, Plan, View, OutputTarget, bPresentOutput, Options,
			SceneTargets, GBufferTargets, SceneColor,
			AmbientOcclusionDebugOutput
		);
	}
} // namespace Durin
