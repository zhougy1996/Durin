#pragma once

#include "SceneOwnership.h"
#include "SceneView.h"
#include "ViewRenderStatistics.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;
	class FSceneInterface;
	struct FRDGCapture;

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
		// Creates an opt-in persistent stream; registry mutation is render-thread ordered.
		virtual auto CreateViewState() -> FSceneViewStateOwner = 0;
		virtual auto InvalidateViewState(FSceneViewStateId Id) -> void = 0;
		virtual auto InvalidateAllViewStates() -> void = 0;
		// A non-null capture output requests one owning graph snapshot for this view.
		virtual auto RenderView(
			FRHICommandListImmediate& CommandList,
			FSceneInterface* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics = nullptr,
			FRDGCapture* OutRenderGraphCapture = nullptr
		) -> ERenderViewResult = 0;
	};
}
