#pragma once

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneRenderGraphTypes.h"
#include "Renderers/SceneRenderFeatureRecorders.h"
#include "RDG.h"

namespace Durin
{
	class FSceneRenderGraphComposer;
	// Owns scene-render preparation, topology selection, and lifecycle finalization.
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
			const FSceneRenderGraphExecute& ExecuteGraph
		) -> ERenderViewResult;

	private:
		auto PrepareView_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			FSceneView& RenderView,
			const FSceneViewRenderOptions& Options
		) -> FSceneRenderPreparationResult;
		auto ResolveSceneRenderResources_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView
		) -> ERenderViewResult;
		auto BuildSceneRenderTopology(
			const FSceneRenderPlan& PreparedView,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height
		) const -> FSceneRenderTopology;

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
		FResolvedSceneResources ResolvedSceneResources;
		FSceneViewTemporalContext TemporalContext;
		FSceneViewState* ViewState = nullptr;
		FSceneRenderFeatureRecorders Recorders;
	};
} // namespace Durin
