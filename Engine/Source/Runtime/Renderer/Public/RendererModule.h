#pragma once
#include "RendererAPI.h"
#include "IRendererModule.h"

namespace Durin
{
	class FSceneRenderer;
	enum class ERendererResourceInvalidationCause : uint8;
	struct FConsoleCommandResult;
	struct FRendererResourceInvalidationSnapshot;

	// Adapts the public renderer-module contract to one private scene renderer.
	class FRendererModule final : public IRendererModule
	{
	public:
		RENDERER_API FRendererModule();
		RENDERER_API ~FRendererModule() override;

		RENDERER_API auto StartupModule() -> void override;
		RENDERER_API auto ShutdownModule() -> void override;
		RENDERER_API auto CreateScene() -> FScenePtr override;
		RENDERER_API auto CreateViewState() -> FSceneViewStateOwner override;
		RENDERER_API auto InvalidateViewState(FSceneViewStateId Id) -> void override;
		RENDERER_API auto InvalidateAllViewStates() -> void override;
		RENDERER_API auto RequestResourceInvalidation(
			ERendererResourceInvalidationCause Cause) -> FConsoleCommandResult;
		RENDERER_API auto GetResourceInvalidationSnapshot_RenderThread() const
			-> FRendererResourceInvalidationSnapshot;
		RENDERER_API auto RenderView(
			FRHICommandListImmediate& CommandList,
			FSceneInterface* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics = nullptr,
			FRDGCapture* OutRenderGraphCapture = nullptr
		) -> ERenderViewResult override;

	private:
		static auto ReleaseViewState(FSceneViewStateId Id) -> void;

		FModuleOwnedCallbackRegistration ConsoleCallbacks;
		std::unique_ptr<FSceneRenderer> SceneRenderer;
	};
}
