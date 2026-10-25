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

	struct FSceneViewRenderRequest
	{
		FSceneInterface* Scene = nullptr;
		FSceneView View;
		FRHITexture* OutputTarget = nullptr;
		bool bPresentOutput = false;
		FSceneViewRenderOptions Options;
		FSceneViewStatistics* Statistics = nullptr;
		FRDGCapture* Capture = nullptr;
		// Observe and bound preparation during this same invocation, then draw the
		// prepared frame directly. Separate PrepareViewResources calls remain prepare-only.
		const FRenderResourcePreparation* Preparation = nullptr;
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
		virtual auto BeginFrame_RenderThread(FRHICommandListImmediate&) -> void {}
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
		virtual auto PrepareViewResources(FRHICommandListImmediate& CommandList,
			const FSceneViewRenderRequest& Request, const FRenderResourcePreparation& Preparation) -> FRenderResourcePreparationProgress
		{ Preparation.Retire_RenderThread(ERenderResourcePreparationState::Failed); return Preparation.GetProgress(); }
		virtual auto RenderView(FRHICommandListImmediate& CommandList,
			const FSceneViewRenderRequest& Request) -> ERenderViewResult = 0;
	};
}
