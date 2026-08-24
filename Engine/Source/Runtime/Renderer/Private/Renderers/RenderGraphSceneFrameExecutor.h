#pragma once

#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRenderer.h"
#include "RenderGraph.h"

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

	// Binds the physical scene-frame contract to graph identities before execution.
	struct FSceneFrameGraphResources
	{
		std::optional<FRenderGraphTextureHandle> DirectionalShadow;
		FRenderGraphTextureHandle SceneColor;
		FRenderGraphTextureHandle SceneDepth;
		FRenderGraphTextureHandle Output;
		std::array<std::optional<FRenderGraphTextureHandle>, 4> GBuffer;
		std::array<std::optional<FRenderGraphTextureHandle>, 4>
			GroundTruthAmbientOcclusion;
		std::optional<FRenderGraphTextureHandle> ContactFragment;
		std::optional<FRenderGraphTextureHandle> ContactCompute;
		std::optional<FRenderGraphTextureHandle> VolumetricCloudShadowFragment;
		std::optional<FRenderGraphTextureHandle> VolumetricCloudShadowCompute;
		std::optional<FRenderGraphTextureHandle> VolumetricCloudBaseDensity;
		std::optional<FRenderGraphTextureHandle> VolumetricCloudDetailDensity;
		std::optional<FRenderGraphTextureHandle> VolumetricCloudWeather;
		std::optional<FRenderGraphTextureHandle> DefaultWhite;
		std::optional<FRenderGraphTextureHandle> DefaultShadowArray;
		std::optional<FRenderGraphTextureHandle> EnvironmentIrradiance;
		std::optional<FRenderGraphTextureHandle> EnvironmentPrefiltered;
		std::optional<FRenderGraphTextureHandle> EnvironmentBrdfLut;
		std::optional<FRenderGraphTextureHandle> VolumetricCloudFragment;
		std::optional<FRenderGraphTextureHandle> VolumetricCloudCompute;
		std::optional<FRenderGraphTextureHandle> VolumetricCloudComposite;
		std::optional<FRenderGraphTextureHandle> IsolatedDeferred;
		std::optional<FRenderGraphTextureHandle> GBufferDebug;
	};

	// Prevents distinct renderer outcomes from sharing an untyped scheduling value.
	template <typename TResult>
	struct TSceneFrameGraphValue
	{
		FRenderGraphTokenHandle Handle;
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
			const FSceneRenderPlan& PreparedView,
			FRHITexture* DirectionalShadowTarget
		) -> FDirectionalShadowPassResult;
		auto RenderGBuffer_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FGBufferRenderer::FTargets* GBufferTargets,
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
			const FGroundTruthAmbientOcclusionRenderer::FTargets*
				AmbientOcclusionTargets,
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
			const FContactShadowVisibilityRenderer::FTargets*
				FragmentContactTargets,
			const FContactShadowVisibilityRenderer::FComputeTargets*
				ComputeContactTargets,
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
			const FVolumetricCloudShadowRenderer::FTargets* FragmentTargets,
			const FVolumetricCloudShadowRenderer::FComputeTargets* ComputeTargets,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			FRHITexture* BaseDensity,
			FRHITexture* DetailDensity,
			FRHITexture* Weather,
			uint32 Width,
			uint32 Height,
			bool bWantsProductionDeferred,
			bool bGBufferComplete
		) -> FVolumetricCloudShadowPassResult;
		auto BuildDeferredParameters(
			const FSceneRenderPlan& PreparedView,
			const FDirectionalShadowPassResult& DirectionalShadow,
			FRHITexture* DirectionalShadowTexture,
			const FGBufferPassResult& GBuffer,
			const FGBufferRenderer::FTargets* GBufferTargets,
			const FGroundTruthAmbientOcclusionPassResult& AmbientOcclusion,
			const FGroundTruthAmbientOcclusionRenderer::FTargets*
				AmbientOcclusionTargets,
			const FContactShadowPassResult& ContactShadow,
			const FContactShadowVisibilityRenderer::FTargets*
				FragmentContactTargets,
			const FContactShadowVisibilityRenderer::FComputeTargets*
				ComputeContactTargets,
			const FVolumetricCloudShadowPassResult& CloudShadow,
			const FVolumetricCloudShadowRenderer::FTargets*
				FragmentCloudShadowTargets,
			const FVolumetricCloudShadowRenderer::FComputeTargets*
				ComputeCloudShadowTargets,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FSceneViewRenderOptions& Options
		) -> std::optional<
			FDeferredDirectionalLightingRenderer::FRenderParameters>;
		auto RenderIsolatedDeferred_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FDeferredDirectionalLightingRenderer::FTargets* Targets,
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
			const FGBufferDebugRenderer::FTargets* GBufferDebugTargets,
			FRHITexture* SceneColor,
			FRHITexture* GroundTruthAmbientOcclusionDebugOutput,
			bool bEditorAssistanceFollows
		) -> FPostProcessPassResult;
		auto RenderEditorAssistance_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			FRHITexture* OutputTarget,
			FRHITexture* DepthTarget,
			bool bPresentOutput,
			const RendererEditorAssistance::FPrepared& Prepared
		) -> bool;
		auto RenderVolumetricCloudSpatial_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			const FVolumetricCloudRenderer::FTargets* FragmentTargets,
			const FVolumetricCloudRenderer::FComputeTargets* ComputeTargets,
			FRHITexture* BaseDensity,
			FRHITexture* DetailDensity,
			FRHITexture* Weather,
			FRHITexture* Depth
		) -> FVolumetricCloudSpatialPassResult;
		auto RenderVolumetricCloudComposite_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			const FVolumetricCloudSpatialPassResult& Spatial,
			const FVolumetricCloudRenderer::FTargets* FragmentTargets,
			const FVolumetricCloudRenderer::FComputeTargets* ComputeTargets,
			const FVolumetricCloudRenderer::FTargets* CompositeTargets,
			FRHITexture* SceneColor,
			FRHITexture* Depth,
			FRHITexture* VolumetricCloudShadowVisibility
		) -> FVolumetricCloudPassResult;
		auto RenderSceneOpaque_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			FRHITexture* SceneColor,
			FRHITexture* Depth,
			const FDeferredDirectionalLightingRenderer::FRenderParameters*
				DeferredParameters
		) -> FSceneColorPassResult;
		auto RenderSceneTranslucency_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneRenderPlan& PreparedView,
			FRHITexture* SceneColor,
			FRHITexture* Depth,
			const FSceneColorPassResult& Opaque,
			const FVolumetricCloudPassResult& VolumetricCloud
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
