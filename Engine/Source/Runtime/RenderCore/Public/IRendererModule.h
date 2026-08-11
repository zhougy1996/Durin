#pragma once

#include "SceneView.h"

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
		virtual auto CreateScene() -> std::unique_ptr<IScene> = 0;
		virtual auto RenderView(
			FRHICommandListImmediate& CommandList,
			IScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options) -> ERenderViewResult = 0;
	};
}
