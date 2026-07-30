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

		RENDERER_API auto StartupModule() -> void override;
		RENDERER_API auto ShutdownModule() -> void override;
		RENDERER_API auto CreateScene() -> std::unique_ptr<IScene> override;
		RENDERER_API auto RenderView(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* OutputTarget, bool bPresentOutput) -> void override;

	private:
		std::unique_ptr<FSceneRenderer> SceneRenderer;
	};
}
