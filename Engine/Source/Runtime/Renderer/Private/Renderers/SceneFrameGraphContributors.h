#pragma once

#include "Renderers/SceneFrameFeatureRecorders.h"
#include "Renderers/SceneFrameGraphComposer.h"
#include "RenderGraph.h"

namespace Durin
{
	struct FSceneFrameGraphComposeInputs;
	struct FSceneFrameGraphComposition;
	struct FSceneFrameGraphServices;

	struct FGBufferPassParameters final
	{
		TRenderGraphValueWrite<FGBufferPassResult> Completion;
		std::array<std::optional<FRenderGraphColorAttachmentParameter>, 4> Colors;
		std::optional<FRenderGraphDepthStencilAttachmentParameter> Depth;

		static RENDERER_API auto GetRenderGraphParametersMetadata()
			-> const FRenderGraphParametersMetadata*;
	};

	struct FContactShadowGraphicsPassParameters final
	{
		TRenderGraphValueRead<FDirectionalShadowPassResult> DirectionalShadow;
		TRenderGraphValueRead<FGBufferPassResult> GBufferCompletion;
		TRenderGraphValueWrite<FContactShadowVisibilityPassResult> Completion;
		std::optional<FRenderGraphTextureParameter> GBufferMaterial;
		std::optional<FRenderGraphTextureParameter> GBufferNormals;
		std::optional<FRenderGraphTextureParameter> GBufferSurface;
		std::optional<FRenderGraphTextureParameter> GBufferEmissive;
		std::optional<FRenderGraphTextureParameter> SceneDepth;
		std::optional<FRenderGraphColorAttachmentParameter> Output;

		static RENDERER_API auto GetRenderGraphParametersMetadata()
			-> const FRenderGraphParametersMetadata*;
	};

	struct FContactShadowComputePassParameters final
	{
		TRenderGraphValueRead<FDirectionalShadowPassResult> DirectionalShadow;
		TRenderGraphValueRead<FGBufferPassResult> GBufferCompletion;
		TRenderGraphValueWrite<FContactShadowVisibilityPassResult> Completion;
		std::optional<FRenderGraphTextureParameter> GBufferMaterial;
		std::optional<FRenderGraphTextureParameter> GBufferNormals;
		std::optional<FRenderGraphTextureParameter> GBufferSurface;
		std::optional<FRenderGraphTextureParameter> GBufferEmissive;
		std::optional<FRenderGraphTextureParameter> SceneDepth;
		std::optional<FRenderGraphTextureParameter> ContactVisibilityOutput;

		static RENDERER_API auto GetRenderGraphParametersMetadata()
			-> const FRenderGraphParametersMetadata*;
	};

	struct FSceneFrameGraphContributorContext final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FSceneFrameGraphComposition& Composition;
		const FSceneView& View;
		FRHITexture* OutputTarget = nullptr;
		const FSceneViewRenderOptions& Options;
		FSceneFrameTopology& Topology;
		const RendererEditorAssistance::FPrepared& EditorAssistance;
		FContactShadowVisibilityRenderer::FRouteDecision ContactRoute;
		FVolumetricCloudShadowRenderer::ERoute CloudShadowRoute;
		FVolumetricCloudRenderer::ERoute CloudRoute;
		FRHITexture* CloudWeatherTexture = nullptr;
		FRHITexture* DirectionalShadowTexture = nullptr;
		FRHISampler* EnvironmentSampler = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		bool bPresentOutput = false;
		bool bHasEditorAssistance = false;
		bool bRequiresDeferredOpaque = false;
		bool bWantsIsolatedDeferred = false;
		bool bWantsGroundTruthAmbientOcclusion = false;
		bool bWantsDeferredInputs = false;
		bool bWantsProductionDeferred = false;
		bool bHybridRetainedResourcesReady = false;
		bool bNeedsGBuffer = false;
	};

#define DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(TypeName, ResultType, PassName, InputType) \
	struct TypeName final \
	{ \
		using Result = ResultType; \
		static constexpr std::string_view Name = PassName; \
		static auto AddPasses(FSceneFrameGraphContributorContext& Context, \
			const InputType& Inputs) -> void; \
	}

	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FDirectionalShadowGraphContributor,
		FDirectionalShadowPassResult, "Scene.DirectionalShadow",
		FDirectionalShadowRecordInputs);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FGBufferGraphContributor,
		FGBufferPassResult, "Scene.GBuffer", FGBufferRecordInputs);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FAmbientOcclusionGraphContributor,
		FGroundTruthAmbientOcclusionPassResult, "Scene.AmbientOcclusion", FSceneView);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FContactShadowVisibilityGraphContributor,
		FContactShadowVisibilityPassResult, "Scene.ContactShadowVisibility",
		FContactShadowVisibilityRecordInputs);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FVolumetricCloudShadowGraphContributor,
		FVolumetricCloudShadowPassResult, "Scene.VolumetricCloudShadow",
		FVolumetricCloudShadowRecordInputs);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(
		FDeferredDirectionalLightingGraphContributor,
		FIsolatedDeferredPassResult, "Scene.DeferredDirectionalLighting",
		FSceneView);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FBaseSceneGraphContributor,
		FSceneColorPassResult, "Scene.Base", FSceneGeometryRecordInputs);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FVolumetricCloudSpatialGraphContributor,
		FVolumetricCloudSpatialPassResult, "Scene.VolumetricCloudSpatial",
		FVolumetricCloudRecordInputs);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FVolumetricCloudCompositeGraphContributor,
		FVolumetricCloudPassResult, "Scene.VolumetricCloud",
		FVolumetricCloudRecordInputs);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FSceneColorGraphContributor,
		FSceneColorPassResult, "Scene.Color", FSceneGeometryRecordInputs);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FPostProcessGraphContributor,
		FPostProcessPassResult, "Scene.PostProcess", FSceneView);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FEditorAssistanceGraphContributor,
		bool, "Scene.EditorAssistance", FSceneView);

#undef DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR

	template <typename TContributor, typename TCallback>
	[[nodiscard]] auto AddSceneFrameFeaturePass(
		FRenderGraphBuilder& Graph,
		ERenderGraphPassType Type,
		TCallback&& Callback) -> FRenderGraphPassHandle
	{
		return Graph.AddPass(TContributor::Name, Type,
			std::forward<TCallback>(Callback));
	}

	template <typename TContributor, CRenderGraphParameters TParameters,
		typename TCallback>
	[[nodiscard]] auto AddSceneFrameFeaturePass(
		FRenderGraphBuilder& Graph,
		ERenderGraphPassType Type,
		TRenderGraphParametersRef<TParameters>&& Parameters,
		TCallback&& Callback) -> FRenderGraphPassHandle
	{
		return Graph.AddPass(TContributor::Name, Type,
			std::move(Parameters), std::forward<TCallback>(Callback));
	}
} // namespace Durin
