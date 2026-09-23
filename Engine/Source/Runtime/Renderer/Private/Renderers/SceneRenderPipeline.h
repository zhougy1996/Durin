#pragma once

#include "Renderers/SceneFrameContext.h"

namespace Durin
{
	class FSceneRenderingService;

	// Owns preparation, graph execution, and lifecycle finalization for
	// one scene-render submission.
	class FSceneRenderPipeline final
	{
	public:
		explicit FSceneRenderPipeline(FSceneRenderingService& Service);

		FSceneRenderPipeline(const FSceneRenderPipeline&) = delete;
		auto operator=(const FSceneRenderPipeline&)
			-> FSceneRenderPipeline& = delete;
		FSceneRenderPipeline(FSceneRenderPipeline&&) = delete;
		auto operator=(FSceneRenderPipeline&&)
			-> FSceneRenderPipeline& = delete;

		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics,
			FRDGCapture* OutRenderGraphCapture
		) -> ERenderViewResult;

	private:
		using FResourcePreparationStage = auto (FSceneRenderPipeline::*)(
			FRHICommandListImmediate&, FSceneFrameContext&) -> ERenderViewResult;
		auto ResolvePipelineStage_RenderThread(FRHICommandListImmediate& CommandList,
			FSceneFrameContext& Context, FResourcePreparationStage Stage) -> ERenderViewResult;
		auto PrepareViewResources_RenderThread(FRHICommandListImmediate& CommandList, FSceneFrameContext& Context) -> ERenderViewResult;
		auto SelectViewState_RenderThread(FSceneFrameContext& Context) -> void;
		auto BeginTemporalState_RenderThread(FSceneFrameContext& Context) -> void;
		auto PrepareFeatureResources_RenderThread(FRHICommandListImmediate& CommandList, FSceneFrameContext& Context) -> ERenderViewResult;
		auto PrepareView_RenderThread(
			FRHICommandListImmediate& CommandList,
			FSceneFrameContext& Context
		) -> FSceneRenderPreparationResult;
		auto ResolveSceneRenderResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			FSceneFrameContext& Context
		) -> ERenderViewResult;
		auto BuildSceneFrameFeaturePlan(
			const FSceneRenderPlan& PreparedView,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			const FRendererQualificationPolicy& Qualification
		) const -> FSceneFrameFeaturePlan;
		auto ExecuteGraph_RenderThread(
			FRDGBuilder& Graph,
			FRHICommandListImmediate& CommandList,
			FRDGCapture* OutRenderGraphCapture
		) -> bool;

		FSceneRenderingService& Service;
	};
} // namespace Durin
