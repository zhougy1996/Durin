#pragma once

#include "CoreMinimal.h"

#include "SceneOwnership.h"
#include "HitProxy.h"
#include "SceneView.h"
#include "ViewRenderStatistics.h"
#include "RHIResources.h"
#include "Shader/ShaderMapPreparation.h"
#include "RenderResourcePreparation.h"
#include "Materials/MaterialPSOPrecache.h"

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
		RequiredEnvironmentUnavailable,
		RendererResourcesPending
	};

	// Defines scene ownership and frame rendering services exposed by the renderer module.
	class IRendererModule : public IModuleInterface
	{
	public:
		virtual auto PrecacheMaterialPSOs(FMaterialPSOPrecacheParams) -> FMaterialPSOPrecacheRequestID
		{ return FMaterialPSOPrecacheRequestID::Rejected("Renderer does not support material PSO precaching."); }
		virtual auto RenderHitProxies(FRHICommandListImmediate&, FSceneInterface*, const FHitProxyRenderRequest&) -> std::shared_ptr<const FHitProxyMap> { return {}; }
		virtual auto CreateScene() -> FScenePtr = 0;
		// Starts CPU-only built-in shader work for the lit scene-thumbnail profile.
		virtual auto PrepareSceneThumbnailShaders() -> FShaderMapPreparation { return {}; }
		// Called inside an active RHI frame, independently of viewport submission.
		virtual auto UpdateScenes_RenderThread(FRHICommandListImmediate&) -> void {}
		// Opt-in GPU graph timing. Set/clear on the render thread; the sink owns
		// each query until its nonblocking completion. Empty disables all overhead.
		virtual auto SetViewGPUTimingSink_RenderThread(std::function<void(FGPUTimingQueryRHIRef)> Sink) -> void {}
		// Creates an opt-in persistent stream; registry mutation is render-thread ordered.
		virtual auto CreateViewState() -> FSceneViewStateOwner = 0;
		virtual auto InvalidateViewState(FSceneViewStateId Id) -> void = 0;
		virtual auto InvalidateAllViewStates() -> void = 0;
		// A non-null capture output requests one owning graph snapshot for this view.
		// Pump on the render thread with the actual upcoming scene/view/output.
		// Pending never joins PSOs or draws; callers retain the handle through activation.
		virtual auto PrepareViewResources(
			FRHICommandListImmediate& CommandList, FSceneInterface* Scene,
			const FSceneView& View, FRHITexture* OutputTarget, bool bPresentOutput,
			const FSceneViewRenderOptions& Options, const FRenderResourcePreparation& Preparation) -> void { Preparation.Retire_RenderThread(ERenderResourcePreparationState::Failed); }
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
