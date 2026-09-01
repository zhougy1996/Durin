#pragma once

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderGraphTypes.h"
#include "RDG.h"

namespace Durin
{
	class FSceneRenderGraphComposer;

	// Owns all submission-local state and keeps policy, resources, transaction,
	// and observation lifetimes distinct.
	struct FSceneFrameContext final
	{
		struct FLogical final
		{
			FScene* Scene = nullptr;
			const FSceneView* CallerView = nullptr;
			FSceneView RenderView;
			FRHITexture* OutputTarget = nullptr;
			FSceneViewRenderOptions Options;
			FRendererQualificationPolicy Qualification;
			std::optional<FSceneRenderPlan> PreparedView;
			RendererEditorAssistance::FPrepared EditorAssistance;
			uint32 Width = 0;
			uint32 Height = 0;
			bool bPresentOutput = false;
			bool bHasEditorAssistance = false;
		};

		struct FResolved final
		{
			FResolvedSceneResources Scene;
			FRHITexture* CloudWeatherTexture = nullptr;
			bool bHybridRetainedResourcesReady = false;
		};

		struct FFeatures final
		{
			FSceneFrameFeaturePlan Plan;
		};

		struct FTransaction final
		{
			FSceneViewTemporalContext Temporal;
			FSceneViewState* ViewState = nullptr;
			FSceneRenderGraphComposition Composition;
		};

		struct FObservation final
		{
			FSceneRenderTelemetry Telemetry;
			bool bReportedRegressionOverage = false;
			bool bReportedExecutionFailure = false;
		};

		FLogical Logical;
		FResolved Resolved;
		FFeatures Features;
		FTransaction Transaction;
		FObservation Observation;
	};

	// Owns preparation, graph authoring/execution, and lifecycle finalization for
	// one scene-render submission.
	class FSceneRenderPipeline final
	{
	public:
		explicit FSceneRenderPipeline(FSceneRenderer& Renderer);

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
		auto PrepareView_RenderThread(
			FRHICommandListImmediate& CommandList,
			FSceneFrameContext& Context
		) -> FSceneRenderPreparationResult;
		auto ResolveSceneRenderResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			FSceneFrameContext& Context
		) -> ERenderViewResult;
		auto BuildSceneFrameFeaturePlan(
			const FSceneRenderPlan& PreparedView,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			const FRendererQualificationPolicy& Qualification
		) const -> FSceneFrameFeaturePlan;
		auto CompileAndExecuteGraph_RenderThread(
			FRDGBuilder& Graph,
			FRHICommandListImmediate& CommandList,
			FRDGCapture* OutRenderGraphCapture,
			FSceneFrameContext::FObservation& Observation
		) -> ESceneRenderGraphExecutionStatus;

		FSceneRenderer& Renderer;
	};
} // namespace Durin
