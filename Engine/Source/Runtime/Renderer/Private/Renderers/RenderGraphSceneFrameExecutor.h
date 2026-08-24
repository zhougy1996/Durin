#pragma once

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRenderer.h"

namespace Durin
{
	struct FSceneFrameRequirements
	{
		uint32 Width = 0;
		uint32 Height = 0;
		bool bGBuffer = false;
		bool bGroundTruthAmbientOcclusion = false;
		bool bContactFragment = false;
		bool bContactCompute = false;
		bool bVolumetricCloudShadowFragment = false;
		bool bVolumetricCloudShadowCompute = false;
		bool bIsolatedDeferred = false;
		bool bGBufferDebug = false;
		bool bVolumetricCloudFragment = false;
		bool bVolumetricCloudCompute = false;
		bool bVolumetricCloudComposite = false;
		EGroundTruthAmbientOcclusionQuality AmbientOcclusionQuality =
			EGroundTruthAmbientOcclusionQuality::FullResolution;
		FIntPoint VolumetricCloudExtent{0, 0};
	};

	struct FResolvedSceneFrameTargets
	{
		std::optional<FPostProcessRenderer::FSceneTargets> Scene;
		std::optional<FGBufferRenderer::FTargets> GBuffer;
		std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
			GroundTruthAmbientOcclusion;
		std::optional<FContactShadowVisibilityRenderer::FTargets>
			ContactFragment;
		std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
			ContactCompute;
		std::optional<FVolumetricCloudShadowRenderer::FTargets>
			VolumetricCloudShadowFragment;
		std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
			VolumetricCloudShadowCompute;
		std::optional<FDeferredDirectionalLightingRenderer::FTargets>
			IsolatedDeferred;
		std::optional<FGBufferDebugRenderer::FTargets> GBufferDebug;
		std::optional<FVolumetricCloudRenderer::FTargets>
			VolumetricCloudFragment;
		std::optional<FVolumetricCloudRenderer::FComputeTargets>
			VolumetricCloudCompute;
		std::optional<FVolumetricCloudRenderer::FTargets>
			VolumetricCloudComposite;
	};

	struct FResolvedSceneFrame
	{
		FResolvedLighting Lighting;
		FResolvedReceiverGeometry Receiver;
		std::optional<FResolvedDirectionalShadow> DirectionalShadow;
		std::optional<FResolvedVolumetricCloud> VolumetricCloud;
		FResolvedSceneFrameTargets Targets;
	};

	// Owns the complete render-graph pass order for one scene-view submission.
	class FRenderGraphSceneFrameExecutor final
	{
	public:
		explicit FRenderGraphSceneFrameExecutor(FSceneRenderer& Renderer);

		FRenderGraphSceneFrameExecutor(const FRenderGraphSceneFrameExecutor&) = delete;
		auto operator=(const FRenderGraphSceneFrameExecutor&)
			-> FRenderGraphSceneFrameExecutor& = delete;
		FRenderGraphSceneFrameExecutor(FRenderGraphSceneFrameExecutor&&) = delete;
		auto operator=(FRenderGraphSceneFrameExecutor&&)
			-> FRenderGraphSceneFrameExecutor& = delete;

		auto Execute_RenderThread(
			FRHICommandListImmediate& CommandList,
			FScene* Scene,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			FSceneViewStatistics* OutStatistics
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
		auto BuildFrameRequirements(
			const FSceneRenderPlan& PreparedView,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height
		) const -> FSceneFrameRequirements;
		auto ResolveFrameTargets_RenderThread(
			const FSceneFrameRequirements& Requirements
		) -> ERenderViewResult;
		auto RenderDirectionalShadow_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView
		) -> FDirectionalShadowPassResult;
		auto RenderGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			bool bNeedsGBuffer,
			bool bWantsIsolatedDeferred
		) -> FGBufferPassResult;
		auto RenderGroundTruthAmbientOcclusion_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			const FGBufferRenderer::FTargets* GBufferTargets,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			bool bWantsGroundTruthAmbientOcclusion,
			bool bGBufferComplete
		) -> FGroundTruthAmbientOcclusionPassResult;
		auto RenderContactShadows_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			const FGBufferRenderer::FTargets* GBufferTargets,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			bool bWantsProductionDeferred,
			bool bGBufferComplete,
			bool bGBufferHasGeometry
		) -> FContactShadowPassResult;
		auto RenderVolumetricCloudShadows_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			uint32 Width,
			uint32 Height,
			bool bWantsProductionDeferred,
			bool bGBufferComplete
		) -> FVolumetricCloudShadowPassResult;
		auto BuildDeferredParameters(
			const FSceneRenderPlan& PreparedView,
			const FDirectionalShadowPassResult& DirectionalShadow,
			const FGBufferPassResult& GBuffer,
			const FGroundTruthAmbientOcclusionPassResult& AmbientOcclusion,
			const FContactShadowPassResult& ContactShadow,
			const FVolumetricCloudShadowPassResult& CloudShadow,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FSceneViewRenderOptions& Options
		) -> std::optional<
			FDeferredDirectionalLightingRenderer::FRenderParameters>;
		auto RenderIsolatedDeferred_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FDeferredDirectionalLightingRenderer::FRenderParameters& DeferredParameters,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			bool bWantsIsolatedDeferred
		) -> FIsolatedDeferredPassResult;
		auto RenderPostProcess_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			const FSceneView& View,
			FRHITexture* OutputTarget,
			bool bPresentOutput,
			const FSceneViewRenderOptions& Options,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FGBufferRenderer::FTargets* GBufferTargets,
			FRHITexture* SceneColor,
			FRHITexture* GroundTruthAmbientOcclusionDebugOutput
		) -> FPostProcessPassResult;
		auto RenderVolumetricCloud_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			FRHITexture* SceneColor,
			FRHITexture* Depth,
			FRHITexture* VolumetricCloudShadowVisibility
		) -> FVolumetricCloudPassResult;
		auto RenderScene_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			FRHITexture* SceneColor,
			FRHITexture* Depth,
			const FDeferredDirectionalLightingRenderer::FRenderParameters*
				DeferredParameters,
			FRHITexture* VolumetricCloudShadowVisibility
		) -> FSceneColorPassResult;
		auto RenderSpecialForwardScene_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			FRHITexture* RenderTarget
		) -> bool;

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
	};
} // namespace Durin
