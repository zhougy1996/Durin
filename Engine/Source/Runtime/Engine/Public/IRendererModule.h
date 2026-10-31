#pragma once

#include "CoreMinimal.h"

#include "SceneOwnership.h"
#include "HitProxy.h"
#include "SceneView.h"
#include "ViewRenderStatistics.h"
#include "RHIResources.h"
#include "RHIFeatureLevel.h"
#include "Shader/ShaderMapPreparation.h"
#include "RenderResourcePreparation.h"
#include "PSOPrecacheMaterial.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;
	class FSceneInterface;
	struct FRDGCapture;

	// Reports whether the admitted scene and selected feature routes produced an output.
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
	};

	// Defines scene ownership and frame rendering services exposed by the renderer module.
	class IRendererModule : public IModuleInterface
	{
	public:
		// GameThread. Renderer owns policy, capability capture and version publication.
		virtual auto GetSceneRenderPreparationConfig(ERHIFeatureLevel) const -> FSceneRenderPreparationConfig { return {}; }
		virtual auto SetPreparationSettings(const FRendererPreparationSettings&) -> bool { return false; }
		virtual auto PrecacheMaterialPSOs(FMaterialPSOPrecacheParams) -> FMaterialPSOPrecacheRequestID
		{ return FMaterialPSOPrecacheRequestID::Rejected("Renderer does not support material PSO precaching."); }
		virtual auto RenderHitProxies(FRHICommandListImmediate&, FSceneInterface*, const FHitProxyRenderRequest&) -> std::shared_ptr<const FHitProxyMap> { return {}; }
		virtual auto CreateScene() -> FScenePtr = 0;
		virtual auto CreateScene(ERHIFeatureLevel FeatureLevel) -> FScenePtr { return {}; }
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
		// Pump on the render thread with the actual upcoming scene/view/output.
		// Pending never joins PSOs or draws; callers retain the handle through activation.
		virtual auto PrepareViewResources(FRHICommandListImmediate& CommandList,
			const FSceneViewRenderRequest& Request, const FRenderResourcePreparation& Preparation) -> FRenderResourcePreparationProgress
		{ Preparation.Retire_RenderThread(ERenderResourcePreparationState::Failed); return Preparation.GetProgress(); }
		// Draw ready components and selected feature fallbacks. Capture owners first
		// wait through PrepareViewResources; Capture requests a graph snapshot only.
		virtual auto RenderView(FRHICommandListImmediate& CommandList,
			const FSceneViewRenderRequest& Request) -> ERenderViewResult = 0;
	};
}
