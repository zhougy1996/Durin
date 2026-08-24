#pragma once

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneFrameGraphTypes.h"
#include "Renderers/SceneFrameFeatureRecorders.h"
#include "RenderGraph.h"

namespace Durin
{
	class FSceneFrameGraphComposer;
	// Owns scene-frame preparation, topology selection, and lifecycle finalization.
	class FSceneFrameExecutionPipeline final
	{
	public:
		explicit FSceneFrameExecutionPipeline(FSceneRenderer& Renderer);

		FSceneFrameExecutionPipeline(const FSceneFrameExecutionPipeline&) = delete;
		auto operator=(const FSceneFrameExecutionPipeline&)
			-> FSceneFrameExecutionPipeline& = delete;
		FSceneFrameExecutionPipeline(FSceneFrameExecutionPipeline&&) = delete;
		auto operator=(FSceneFrameExecutionPipeline&&)
			-> FSceneFrameExecutionPipeline& = delete;

		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics,
			const FSceneFrameGraphExecute& ExecuteGraph
		) -> ERenderViewResult;

	private:
		auto PrepareView_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			FSceneView& RenderView,
			const FSceneViewRenderOptions& Options
		) -> FSceneFramePreparationResult;
		auto ResolveFrameResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView
		) -> ERenderViewResult;
		auto BuildFrameTopology(
			const FSceneRenderPlan& PreparedView,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height
		) const -> FSceneFrameTopology;
		auto ResolveFrameTargets_RenderThread(
			const FSceneFrameTopology& Topology
		) -> ERenderViewResult;

		FDefaultTextureResources& DefaultTextures;
		FEnvironmentLightingResources& EnvironmentLighting;
		FDirectionalShadowRenderer& DirectionalShadowRenderer;
		FGBufferRenderer& GBufferRenderer;
		FGBufferDebugRenderer& GBufferDebugRenderer;
		FDeferredDirectionalLightingRenderer& DeferredDirectionalLightingRenderer;
		FGroundTruthAmbientOcclusionRenderer& GroundTruthAmbientOcclusionRenderer;
		FStaticMeshRenderer& StaticMeshRenderer;
		FTerrainRenderer& TerrainRenderer;
		FSkeletalMeshRenderer& SkeletalMeshRenderer;
		FSkyBoxRenderer& SkyBoxRenderer;
		FPostProcessRenderer& PostProcessRenderer;
		FContactShadowVisibilityRenderer& ContactShadowRenderer;
		FVolumetricCloudRenderer& VolumetricCloudRenderer;
		FVolumetricCloudShadowRenderer& VolumetricCloudShadowRenderer;
		FEditorAssistanceRenderer& EditorAssistanceRenderer;
		FSceneViewStateRegistry& ViewStates;
		uint64& RenderSubmissionSerial;
		FRendererQualificationPolicy Qualification;
		FSceneRenderTelemetry Telemetry;
		FResolvedSceneFrame ResolvedFrame;
		FSceneViewTemporalContext TemporalContext;
		FSceneViewState* ViewState = nullptr;
		FSceneFrameFeatureRecorders Recorders;
	};
} // namespace Durin
