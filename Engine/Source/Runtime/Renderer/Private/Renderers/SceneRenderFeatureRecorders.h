#pragma once

#include "Renderers/SceneRenderGraphTypes.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderer.h"

namespace Durin
{
	class FRDGShaderParameterScope;
	struct FDirectionalShadowRecordInputs final
	{
		const FPreparedDirectionalShadow* Shadow = nullptr;
	};

	struct FGBufferRecordInputs final
	{
		const FSceneView& View;
		const FPreparedReceiverGeometry& Receiver;
	};

	struct FContactShadowVisibilityRecordInputs final
	{
		const FSceneView& View;
		const FPreparedDirectionalShadow* Shadow = nullptr;
	};

	struct FVolumetricCloudShadowRecordInputs final
	{
		const FSceneView& View;
		const FPreparedVolumetricCloud* Cloud = nullptr;
		const FPreparedLighting& Lighting;
	};

	struct FVolumetricCloudRecordInputs final
	{
		const FSceneView& View;
		const FPreparedVolumetricCloud* Cloud = nullptr;
	};

	struct FSceneGeometryRecordInputs final
	{
		const FSceneView& View;
		const FPreparedEnvironment* Environment = nullptr;
		const FPreparedReceiverGeometry& Receiver;
	};

	// Coordinates feature command recording through borrowed renderer services.
	class FSceneRenderFeatureRecorders final
	{
	public:
		FSceneRenderFeatureRecorders(
			FSceneRenderer& Renderer,
			FSceneRenderTelemetry& Telemetry,
			FResolvedSceneResources& ResolvedFrame,
			FSceneViewTemporalContext& TemporalContext,
			FSceneViewState*& ViewState);

	auto RenderDirectionalShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FDirectionalShadowRecordInputs& Inputs,
		FRHITexture* DirectionalShadowTarget
	) -> FDirectionalShadowPassResult;
	auto RenderGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FGBufferRecordInputs& Inputs,
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
		const FSceneView& RenderView,
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
	auto RenderContactShadowVisibility_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FContactShadowVisibilityRecordInputs& Inputs,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FContactShadowVisibilityRenderer::FTargets*
			FragmentContactTargets,
		const FContactShadowVisibilityRenderer::FComputeTargets*
			ComputeContactTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FRDGShaderParameterScope* ShaderParameters,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete,
		bool bGBufferHasGeometry
	) -> FContactShadowVisibilityPassResult;
	auto RenderVolumetricCloudShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudShadowRecordInputs& Inputs,
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
		const FSceneView& RenderView,
		FRHITexture* EnvironmentIrradiance,
		FRHITexture* EnvironmentPrefiltered,
		FRHITexture* EnvironmentBrdfLut,
		FRHISampler* EnvironmentSampler,
		const FDirectionalShadowPassResult& DirectionalShadow,
		FRHITexture* DirectionalShadowTexture,
		const FGBufferPassResult& GBuffer,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FGroundTruthAmbientOcclusionPassResult& AmbientOcclusion,
		const FGroundTruthAmbientOcclusionRenderer::FTargets*
			AmbientOcclusionTargets,
		const FContactShadowVisibilityPassResult& ContactShadow,
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
		const FSceneView& RenderView,
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
		const FSceneView& RenderView,
		FRHITexture* OutputTarget,
		FRHITexture* DepthTarget,
		bool bPresentOutput,
		const RendererEditorAssistance::FPrepared& Prepared
	) -> bool;
	auto RenderVolumetricCloudSpatial_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudRecordInputs& Inputs,
		const FVolumetricCloudRenderer::FTargets* FragmentTargets,
		const FVolumetricCloudRenderer::FComputeTargets* ComputeTargets,
		FRHITexture* BaseDensity,
		FRHITexture* DetailDensity,
		FRHITexture* Weather,
		FRHITexture* Depth
	) -> FVolumetricCloudSpatialPassResult;
	auto RenderVolumetricCloudComposite_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudRecordInputs& Inputs,
		const FVolumetricCloudSpatialPassResult& Spatial,
		const FVolumetricCloudRenderer::FTargets* FragmentTargets,
		const FVolumetricCloudRenderer::FComputeTargets* ComputeTargets,
		const FVolumetricCloudRenderer::FTargets* CompositeTargets,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		FRHITexture* VolumetricCloudShadowVisibility
	) -> FVolumetricCloudPassResult;
	auto RenderBaseScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneGeometryRecordInputs& Inputs,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		const FDeferredDirectionalLightingRenderer::FRenderParameters*
			DeferredParameters
	) -> FSceneColorPassResult;
	auto RenderSceneTranslucency_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneGeometryRecordInputs& Inputs,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		const FSceneColorPassResult& BaseScene,
		const FVolumetricCloudPassResult& VolumetricCloud
	) -> FSceneColorPassResult;
	auto RenderForwardScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneGeometryRecordInputs& Inputs,
		FRHITexture* RenderTarget
	) -> bool;

	private:
		FRendererTransientTargetPool& TransientTargets;
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
		FRendererQualificationPolicy Qualification;
		FSceneRenderTelemetry& Telemetry;
		FResolvedSceneResources& ResolvedFrame;
		FSceneViewTemporalContext& TemporalContext;
		FSceneViewState*& ViewState;
	};
} // namespace Durin
