#pragma once

#include "Renderers/SceneFrameFeatureRecorders.h"
#include "Renderers/SceneFrameGraphComposer.h"
#include "RenderGraph.h"

namespace Durin
{
	struct FSceneFrameGraphComposeInputs;
	struct FSceneFrameGraphComposition;
	struct FSceneFrameGraphServices;

#define DURIN_DECLARE_SCENE_PASS_RESOURCES(TypeName, ...) \
	struct TypeName final \
	{ \
		__VA_ARGS__ \
		static RENDERER_API auto GetRenderGraphParametersMetadata() \
			-> const FRenderGraphParametersMetadata*; \
	}

	DURIN_DECLARE_SCENE_PASS_RESOURCES(FDirectionalShadowPassResources,
		std::optional<FRenderGraphDepthStencilAttachmentParameter>
			DirectionalShadowOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FAmbientOcclusionPassResources,
		std::array<std::optional<FRenderGraphTextureParameter>, 4> GBuffer;
		std::optional<FRenderGraphTextureParameter> SceneDepth;
		std::array<std::optional<FRenderGraphManagedTextureParameter>, 4>
			AmbientOcclusionManaged;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FVolumetricCloudShadowPassResources,
		std::optional<FRenderGraphTextureParameter> SceneDepth;
		std::optional<FRenderGraphTextureParameter> SceneDepthCompute;
		std::optional<FRenderGraphTextureParameter> CloudBaseDensity;
		std::optional<FRenderGraphTextureParameter> CloudDetailDensity;
		std::optional<FRenderGraphTextureParameter> CloudWeather;
		std::optional<FRenderGraphTextureParameter> CloudBaseDensityCompute;
		std::optional<FRenderGraphTextureParameter> CloudDetailDensityCompute;
		std::optional<FRenderGraphTextureParameter> CloudWeatherCompute;
		std::optional<FRenderGraphManagedTextureParameter> CloudShadowFragmentOutput;
		std::optional<FRenderGraphTextureParameter> CloudShadowComputeOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FDeferredDirectionalLightingPassResources,
		std::optional<FRenderGraphTextureParameter> DirectionalShadow;
		std::array<std::optional<FRenderGraphTextureParameter>, 4> GBuffer;
		std::optional<FRenderGraphTextureParameter> SceneDepth;
		std::array<std::optional<FRenderGraphTextureParameter>, 4> AmbientOcclusion;
		std::optional<FRenderGraphTextureParameter> ContactShadowFragment;
		std::optional<FRenderGraphTextureParameter> ContactShadowCompute;
		std::optional<FRenderGraphTextureParameter> CloudShadowFragment;
		std::optional<FRenderGraphTextureParameter> CloudShadowCompute;
		std::optional<FRenderGraphTextureParameter> DefaultWhite;
		std::optional<FRenderGraphTextureParameter> DefaultShadowArray;
		std::optional<FRenderGraphTextureParameter> EnvironmentIrradiance;
		std::optional<FRenderGraphTextureParameter> EnvironmentPrefiltered;
		std::optional<FRenderGraphTextureParameter> EnvironmentBrdfLut;
		std::optional<FRenderGraphColorAttachmentParameter> IsolatedDeferredOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FBaseScenePassResources,
		std::optional<FRenderGraphTextureParameter> DirectionalShadow;
		std::optional<FRenderGraphTextureParameter> DefaultWhite;
		std::optional<FRenderGraphTextureParameter> DefaultShadowArray;
		std::optional<FRenderGraphTextureParameter> EnvironmentIrradiance;
		std::optional<FRenderGraphTextureParameter> EnvironmentPrefiltered;
		std::optional<FRenderGraphTextureParameter> EnvironmentBrdfLut;
		std::optional<FRenderGraphColorAttachmentParameter> SceneColorOutput;
		std::optional<FRenderGraphManagedTextureParameter> SceneDepthGraphicsToGraphics;
		std::optional<FRenderGraphManagedTextureParameter> SceneDepthGraphicsToDepth;
		std::optional<FRenderGraphManagedTextureParameter> SceneDepthDepthToGraphics;
		std::optional<FRenderGraphManagedTextureParameter> SceneDepthDepthToDepth;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FVolumetricCloudSpatialPassResources,
		std::optional<FRenderGraphTextureParameter> SceneDepth;
		std::optional<FRenderGraphTextureParameter> SceneDepthCompute;
		std::optional<FRenderGraphTextureParameter> CloudBaseDensity;
		std::optional<FRenderGraphTextureParameter> CloudDetailDensity;
		std::optional<FRenderGraphTextureParameter> CloudWeather;
		std::optional<FRenderGraphTextureParameter> CloudBaseDensityCompute;
		std::optional<FRenderGraphTextureParameter> CloudDetailDensityCompute;
		std::optional<FRenderGraphTextureParameter> CloudWeatherCompute;
		std::optional<FRenderGraphManagedTextureParameter> CloudFragmentOutput;
		std::optional<FRenderGraphTextureParameter> CloudComputeOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FVolumetricCloudCompositePassResources,
		std::optional<FRenderGraphTextureParameter> SceneColor;
		std::optional<FRenderGraphTextureParameter> SceneDepth;
		std::optional<FRenderGraphTextureParameter> CloudBaseDensity;
		std::optional<FRenderGraphTextureParameter> CloudDetailDensity;
		std::optional<FRenderGraphTextureParameter> CloudWeather;
		std::optional<FRenderGraphTextureParameter> CloudShadowFragment;
		std::optional<FRenderGraphTextureParameter> CloudShadowCompute;
		std::optional<FRenderGraphTextureParameter> CloudFragment;
		std::optional<FRenderGraphTextureParameter> CloudCompute;
		std::optional<FRenderGraphManagedTextureParameter> CloudCompositeOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FSceneColorPassResources,
		std::optional<FRenderGraphManagedTextureParameter> SceneColorManaged;
		std::optional<FRenderGraphManagedTextureParameter> SceneDepthManaged;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FPostProcessPassResources,
		std::optional<FRenderGraphColorAttachmentParameter> OutputPresent;
		std::optional<FRenderGraphColorAttachmentParameter> OutputOffscreen;
		std::optional<FRenderGraphColorAttachmentParameter> OutputForEditor;
		std::optional<FRenderGraphTextureParameter> SceneColor;
		std::optional<FRenderGraphTextureParameter> CloudComposite;
		std::optional<FRenderGraphTextureParameter> SceneDepth;
		std::optional<FRenderGraphColorAttachmentParameter> GBufferDebugOutput;
		std::array<std::optional<FRenderGraphTextureParameter>, 4> GBuffer;
		std::optional<FRenderGraphTextureParameter> IsolatedDeferred;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FEditorAssistancePassResources,
		std::optional<FRenderGraphColorAttachmentParameter> EditorOutputPresent;
		std::optional<FRenderGraphColorAttachmentParameter> EditorOutputOffscreen;
		std::optional<FRenderGraphDepthStencilAttachmentParameter> EditorDepth;);

#undef DURIN_DECLARE_SCENE_PASS_RESOURCES

#define DURIN_DECLARE_SCENE_PASS_PARAMETERS(TypeName, ResourceType, Members) \
	struct TypeName final \
	{ \
		Members \
		ResourceType Resources; \
		static RENDERER_API auto GetRenderGraphParametersMetadata() \
			-> const FRenderGraphParametersMetadata*; \
	}

	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FDirectionalShadowPassParameters,
		FDirectionalShadowPassResources,
		TRenderGraphValueWrite<FDirectionalShadowPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FAmbientOcclusionPassParameters,
		FAmbientOcclusionPassResources,
		TRenderGraphValueRead<FGBufferPassResult> GBufferCompletion;
		TRenderGraphValueWrite<FGroundTruthAmbientOcclusionPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FVolumetricCloudShadowPassParameters,
		FVolumetricCloudShadowPassResources,
		TRenderGraphValueRead<FGBufferPassResult> GBufferCompletion;
		TRenderGraphValueWrite<FVolumetricCloudShadowPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FDeferredDirectionalLightingPassParameters,
		FDeferredDirectionalLightingPassResources,
		TRenderGraphValueRead<FDirectionalShadowPassResult> DirectionalShadow;
		TRenderGraphValueRead<FGBufferPassResult> GBufferCompletion;
		TRenderGraphValueRead<FGroundTruthAmbientOcclusionPassResult> AmbientOcclusion;
		TRenderGraphValueRead<FContactShadowVisibilityPassResult> ContactShadow;
		TRenderGraphValueRead<FVolumetricCloudShadowPassResult> CloudShadow;
		TRenderGraphValueWrite<FIsolatedDeferredPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FBaseScenePassParameters,
		FBaseScenePassResources,
		TRenderGraphValueRead<FIsolatedDeferredPassResult> DeferredLighting;
		TRenderGraphValueWrite<FSceneColorPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FVolumetricCloudSpatialPassParameters,
		FVolumetricCloudSpatialPassResources,
		TRenderGraphValueRead<FSceneColorPassResult> BaseScene;
		TRenderGraphValueWrite<FVolumetricCloudSpatialPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FVolumetricCloudCompositePassParameters,
		FVolumetricCloudCompositePassResources,
		TRenderGraphValueRead<FSceneColorPassResult> BaseScene;
		TRenderGraphValueRead<FVolumetricCloudSpatialPassResult> Spatial;
		TRenderGraphValueRead<FVolumetricCloudShadowPassResult> CloudShadow;
		TRenderGraphValueWrite<FVolumetricCloudPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FSceneColorPassParameters,
		FSceneColorPassResources,
		TRenderGraphValueRead<FSceneColorPassResult> BaseScene;
		TRenderGraphValueRead<FVolumetricCloudPassResult> VolumetricCloud;
		TRenderGraphValueWrite<FSceneColorPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FPostProcessPassParameters,
		FPostProcessPassResources,
		TRenderGraphValueRead<FSceneColorPassResult> SceneColor;
		TRenderGraphValueRead<FGBufferPassResult> GBufferCompletion;
		TRenderGraphValueRead<FIsolatedDeferredPassResult> DeferredLighting;
		TRenderGraphValueWrite<FPostProcessPassResult> Completion;
		std::optional<FRenderGraphTokenParameter> OutputCompletion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FEditorAssistancePassParameters,
		FEditorAssistancePassResources,
		TRenderGraphValueRead<FPostProcessPassResult> PostProcess;
		FRenderGraphTokenParameter OutputCompletion;);

#undef DURIN_DECLARE_SCENE_PASS_PARAMETERS

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

	template <typename TResult>
	struct TSceneGraphFeatureOutput
	{
		TRenderGraphValueHandle<TResult> Completion;
	};

	struct FDirectionalShadowGraphOutput final
	{
		TRenderGraphValueHandle<FDirectionalShadowPassResult> Completion;
		std::optional<FRenderGraphTextureHandle> Shadow;
	};
	struct FGBufferGraphOutput final
	{
		TRenderGraphValueHandle<FGBufferPassResult> Completion;
		std::array<std::optional<FRenderGraphTextureHandle>, 4> Textures;
		FRenderGraphTextureHandle Depth;
	};
	struct FAmbientOcclusionGraphOutput final
	{
		TRenderGraphValueHandle<FGroundTruthAmbientOcclusionPassResult> Completion;
		std::array<std::optional<FRenderGraphTextureHandle>, 4> Textures;
		EGroundTruthAmbientOcclusionQuality Quality =
			EGroundTruthAmbientOcclusionQuality::FullResolution;
	};
	struct FContactShadowGraphOutput final
	{
		TRenderGraphValueHandle<FContactShadowVisibilityPassResult> Completion;
		std::optional<FRenderGraphTextureHandle> Fragment;
		std::optional<FRenderGraphTextureHandle> Compute;
	};
	struct FCloudShadowGraphOutput final
	{
		TRenderGraphValueHandle<FVolumetricCloudShadowPassResult> Completion;
		std::optional<FRenderGraphTextureHandle> Fragment;
		std::optional<FRenderGraphTextureHandle> Compute;
	};
	struct FDeferredLightingGraphOutput final
	{
		TRenderGraphValueHandle<FIsolatedDeferredPassResult> Completion;
		std::optional<FRenderGraphTextureHandle> Isolated;
	};
	struct FBaseSceneGraphOutput final
	{
		TRenderGraphValueHandle<FSceneColorPassResult> Completion;
		FRenderGraphTextureHandle Color;
		FRenderGraphTextureHandle Depth;
	};
	struct FCloudSpatialGraphOutput final
	{
		TRenderGraphValueHandle<FVolumetricCloudSpatialPassResult> Completion;
		std::optional<FRenderGraphTextureHandle> Fragment;
		std::optional<FRenderGraphTextureHandle> Compute;
		std::optional<FRenderGraphTextureHandle> Composite;
	};
	struct FCloudCompositeGraphOutput final
	{
		TRenderGraphValueHandle<FVolumetricCloudPassResult> Completion;
		std::optional<FRenderGraphTextureHandle> Composite;
	};
	struct FSceneColorGraphOutput final
	{
		TRenderGraphValueHandle<FSceneColorPassResult> Completion;
		FRenderGraphTextureHandle Color;
		FRenderGraphTextureHandle Depth;
		std::optional<FRenderGraphTextureHandle> CloudComposite;
	};
	struct FPostProcessGraphOutput final
	{
		TRenderGraphValueHandle<FPostProcessPassResult> Completion;
		FRenderGraphTextureHandle Output;
		FRenderGraphTokenHandle OutputCompletion;
	};
	struct FEditorAssistanceGraphOutput final
	{
		FRenderGraphTokenHandle OutputCompletion;
	};

	struct FDirectionalShadowGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FDirectionalShadowRecordInputs Record;
		std::optional<FRenderGraphTextureHandle> Shadow;
	};
	struct FGBufferGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FGBufferRecordInputs Record;
		FRenderGraphTextureHandle Depth;
		const FSceneViewRenderOptions& Options;
		uint32 Width;
		uint32 Height;
		bool bEnabled;
		bool bNeedsGBuffer;
		bool bWantsIsolatedDeferred;
	};
	struct FAmbientOcclusionGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		const FSceneView& View;
		const FSceneViewRenderOptions& Options;
		const FGBufferGraphOutput& GBuffer;
		uint32 Width;
		uint32 Height;
		bool bEnabled;
		bool bRequested;
		EGroundTruthAmbientOcclusionQuality Quality;
	};
	struct FContactShadowGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FContactShadowVisibilityRecordInputs Record;
		const FSceneViewRenderOptions& Options;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		const FGBufferGraphOutput& GBuffer;
		FContactShadowVisibilityRenderer::FRouteDecision Route;
		ESceneFrameRoute GraphRoute;
		uint32 Width;
		uint32 Height;
		bool bProductionDeferred;
	};
	struct FCloudShadowGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FVolumetricCloudShadowRecordInputs Record;
		const FGBufferGraphOutput& GBuffer;
		FRenderGraphTextureHandle SceneDepth;
		std::optional<FRenderGraphTextureHandle> BaseDensity;
		std::optional<FRenderGraphTextureHandle> DetailDensity;
		std::optional<FRenderGraphTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		FVolumetricCloudShadowRenderer::ERoute Route;
		ESceneFrameRoute GraphRoute;
		uint32 Width;
		uint32 Height;
		bool bProductionDeferred;
	};
	struct FDeferredLightingGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		const FSceneView& View;
		const FSceneViewRenderOptions& Options;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		const FGBufferGraphOutput& GBuffer;
		const FAmbientOcclusionGraphOutput& AmbientOcclusion;
		const FContactShadowGraphOutput& ContactShadow;
		const FCloudShadowGraphOutput& CloudShadow;
		std::optional<FRenderGraphTextureHandle> DefaultWhite;
		std::optional<FRenderGraphTextureHandle> DefaultShadowArray;
		std::optional<FRenderGraphTextureHandle> EnvironmentIrradiance;
		std::optional<FRenderGraphTextureHandle> EnvironmentPrefiltered;
		std::optional<FRenderGraphTextureHandle> EnvironmentBrdfLut;
		FRHITexture* SelectedEnvironmentIrradiance;
		FRHITexture* SelectedEnvironmentPrefiltered;
		FRHITexture* SelectedEnvironmentBrdfLut;
		FRHISampler* EnvironmentSampler;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>&
			DeferredParameters;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>&
			ProductionDeferredParameters;
		uint32 Width;
		uint32 Height;
		bool bIsolated;
		bool bWantsDeferredInputs;
		bool bWantsIsolatedDeferred;
		bool bWantsProductionDeferred;
		bool bHybridRetainedResourcesReady;
	};
	struct FBaseSceneGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FSceneGeometryRecordInputs Record;
		const FDeferredLightingGraphOutput& Deferred;
		FRenderGraphTextureHandle SceneColor;
		FRenderGraphTextureHandle SceneDepth;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		std::optional<FRenderGraphTextureHandle> DefaultWhite;
		std::optional<FRenderGraphTextureHandle> DefaultShadowArray;
		std::optional<FRenderGraphTextureHandle> EnvironmentIrradiance;
		std::optional<FRenderGraphTextureHandle> EnvironmentPrefiltered;
		std::optional<FRenderGraphTextureHandle> EnvironmentBrdfLut;
		FRHITexture* SelectedEnvironmentIrradiance;
		FRHITexture* SelectedEnvironmentPrefiltered;
		FRHITexture* SelectedEnvironmentBrdfLut;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>&
			ProductionDeferredParameters;
		bool bRequiresDeferredOpaque;
		bool bNeedsGBuffer;
	};
	struct FCloudSpatialGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FVolumetricCloudRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		std::optional<FRenderGraphTextureHandle> BaseDensity;
		std::optional<FRenderGraphTextureHandle> DetailDensity;
		std::optional<FRenderGraphTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		FVolumetricCloudRenderer::ERoute Route;
		ESceneFrameRoute GraphRoute;
		FIntPoint Extent;
		uint32 Width;
		uint32 Height;
		bool bComposite;
	};
	struct FCloudCompositeGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FVolumetricCloudRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		const FCloudSpatialGraphOutput& Spatial;
		const FCloudShadowGraphOutput& CloudShadow;
		std::optional<FRenderGraphTextureHandle> BaseDensity;
		std::optional<FRenderGraphTextureHandle> DetailDensity;
		std::optional<FRenderGraphTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		bool bEnabled;
	};
	struct FSceneColorGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		FSceneGeometryRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		const FCloudCompositeGraphOutput& VolumetricCloud;
		FSceneColorPassResult& Publication;
		bool bRequiresDeferredOpaque;
		bool bVolumetricCloudComposite;
	};
	struct FPostProcessGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		const FSceneView& RecordView;
		const FSceneView& View;
		const FSceneViewRenderOptions& Options;
		const FSceneColorGraphOutput& SceneColor;
		const FGBufferGraphOutput& GBuffer;
		const FDeferredLightingGraphOutput& Deferred;
		FRenderGraphTextureHandle Output;
		FRHITexture* OutputTarget;
		FPostProcessPassResult& Publication;
		uint32 Width;
		uint32 Height;
		bool bGBufferDebug;
		bool bPresentOutput;
		bool bHasEditorAssistance;
	};
	struct FEditorAssistanceGraphInputs final
	{
		FRenderGraphBuilder& Graph;
		FSceneFrameGraphServices& Services;
		const FSceneView& View;
		const RendererEditorAssistance::FPrepared& Prepared;
		const FPostProcessGraphOutput& PostProcess;
		FRenderGraphTextureHandle SceneDepth;
		FRHITexture* OutputTarget;
		FPostProcessPassResult& Publication;
		bool bPresentOutput;
		bool bEnabled;
	};

#define DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(TypeName, ResultType, PassName, InputType, OutputType) \
	struct TypeName final \
	{ \
		using Result = ResultType; \
		static constexpr std::string_view Name = PassName; \
		static auto AddPasses(const InputType& Inputs) -> OutputType; \
	}

	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FDirectionalShadowGraphContributor,
		FDirectionalShadowPassResult, "Scene.DirectionalShadow",
		FDirectionalShadowGraphInputs, FDirectionalShadowGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FGBufferGraphContributor,
		FGBufferPassResult, "Scene.GBuffer", FGBufferGraphInputs,
		FGBufferGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FAmbientOcclusionGraphContributor,
		FGroundTruthAmbientOcclusionPassResult, "Scene.AmbientOcclusion",
		FAmbientOcclusionGraphInputs, FAmbientOcclusionGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FContactShadowVisibilityGraphContributor,
		FContactShadowVisibilityPassResult, "Scene.ContactShadowVisibility",
		FContactShadowGraphInputs, FContactShadowGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FVolumetricCloudShadowGraphContributor,
		FVolumetricCloudShadowPassResult, "Scene.VolumetricCloudShadow",
		FCloudShadowGraphInputs, FCloudShadowGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(
		FDeferredDirectionalLightingGraphContributor,
		FIsolatedDeferredPassResult, "Scene.DeferredDirectionalLighting",
		FDeferredLightingGraphInputs, FDeferredLightingGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FBaseSceneGraphContributor,
		FSceneColorPassResult, "Scene.Base", FBaseSceneGraphInputs,
		FBaseSceneGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FVolumetricCloudSpatialGraphContributor,
		FVolumetricCloudSpatialPassResult, "Scene.VolumetricCloudSpatial",
		FCloudSpatialGraphInputs, FCloudSpatialGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FVolumetricCloudCompositeGraphContributor,
		FVolumetricCloudPassResult, "Scene.VolumetricCloud",
		FCloudCompositeGraphInputs, FCloudCompositeGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FSceneColorGraphContributor,
		FSceneColorPassResult, "Scene.Color", FSceneColorGraphInputs,
		FSceneColorGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FPostProcessGraphContributor,
		FPostProcessPassResult, "Scene.PostProcess", FPostProcessGraphInputs,
		FPostProcessGraphOutput);
	DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR(FEditorAssistanceGraphContributor,
		bool, "Scene.EditorAssistance", FEditorAssistanceGraphInputs,
		FEditorAssistanceGraphOutput);

#undef DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR

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
