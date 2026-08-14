#pragma once
#include "RendererAPI.h"
#include "IRendererModule.h"

namespace Durin
{
	class FSceneRenderer;

	// Adapts the public renderer-module contract to one private scene renderer.
	class FRendererModule final : public IRendererModule
	{
	public:
		RENDERER_API FRendererModule();
		RENDERER_API ~FRendererModule() override;

		RENDERER_API auto StartupModule(FModuleContext& Context) -> void override;
		RENDERER_API auto ShutdownModule(FModuleShutdownContext& Context) -> void override;
		RENDERER_API auto CreateScene() -> FScenePtr override;
		RENDERER_API auto RenderView(
			FRHICommandListImmediate& CommandList,
			IScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics = nullptr) -> ERenderViewResult override;

	private:
		FModuleOwnedCallbackRegistration ConsoleCallbacks;
		std::unique_ptr<FSceneRenderer> SceneRenderer;
	};
}
