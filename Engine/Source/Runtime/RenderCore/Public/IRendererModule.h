#pragma once

#include "SceneOwnership.h"
#include "SceneView.h"
#include "ViewRenderStatistics.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;
	class IScene;

	// Reports whether one render-thread view submission produced a complete output.
	enum class ERenderViewResult : uint8
	{
		Success,
		InvalidOutput,
		RendererResourcesUnavailable,
		RequiredEnvironmentUnavailable
	};

	// Defines scene ownership and frame rendering services exposed by the renderer module.
	class IRendererModule : public IModuleInterface
	{
	public:
		virtual auto CreateScene() -> FScenePtr = 0;
		virtual auto RenderView(
			FRHICommandListImmediate& CommandList,
			IScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics = nullptr) -> ERenderViewResult = 0;
	};
}
