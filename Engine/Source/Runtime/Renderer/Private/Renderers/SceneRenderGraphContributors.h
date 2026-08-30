#pragma once

#include "Renderers/SceneRenderFeatureRecorders.h"
#include "Renderers/SceneRenderGraphComposer.h"
#include "RDG.h"

namespace Durin
{
	struct FSceneRenderGraphComposeInputs;
	struct FSceneRenderGraphComposition;
	struct FSceneRenderGraphServices;

#define DURIN_DECLARE_SCENE_PASS_RESOURCES(TypeName, ...) \
	struct TypeName final \
	{ \
		__VA_ARGS__ \
		static RENDERER_API auto GetRDGParametersMetadata() \
			-> const FRDGParametersMetadata*; \
	}

	DURIN_DECLARE_SCENE_PASS_RESOURCES(FDirectionalShadowPassResources,
		std::optional<FRDGDepthStencilAttachmentParameter>
			DirectionalShadowOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FAmbientOcclusionPassResources,
		std::array<std::optional<FRDGTextureParameter>, 4> GBuffer;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::array<std::optional<FRDGManagedTextureParameter>, 4>
			AmbientOcclusionManaged;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FVolumetricCloudShadowPassResources,
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGTextureParameter> SceneDepthCompute;
		std::optional<FRDGTextureParameter> CloudBaseDensity;
		std::optional<FRDGTextureParameter> CloudDetailDensity;
		std::optional<FRDGTextureParameter> CloudWeather;
		std::optional<FRDGTextureParameter> CloudBaseDensityCompute;
		std::optional<FRDGTextureParameter> CloudDetailDensityCompute;
		std::optional<FRDGTextureParameter> CloudWeatherCompute;
		std::optional<FRDGManagedTextureParameter> CloudShadowFragmentOutput;
		std::optional<FRDGTextureParameter> CloudShadowComputeOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FDeferredDirectionalLightingPassResources,
		std::optional<FRDGTextureParameter> DirectionalShadow;
		std::array<std::optional<FRDGTextureParameter>, 4> GBuffer;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::array<std::optional<FRDGTextureParameter>, 4> AmbientOcclusion;
		std::optional<FRDGTextureParameter> ContactShadowFragment;
		std::optional<FRDGTextureParameter> ContactShadowCompute;
		std::optional<FRDGTextureParameter> CloudShadowFragment;
		std::optional<FRDGTextureParameter> CloudShadowCompute;
		std::optional<FRDGTextureParameter> DefaultWhite;
		std::optional<FRDGTextureParameter> DefaultShadowArray;
		std::optional<FRDGTextureParameter> EnvironmentIrradiance;
		std::optional<FRDGTextureParameter> EnvironmentPrefiltered;
		std::optional<FRDGTextureParameter> EnvironmentBrdfLut;
		std::optional<FRDGColorAttachmentParameter> IsolatedDeferredOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FBaseScenePassResources,
		std::optional<FRDGTextureParameter> DirectionalShadow;
		std::optional<FRDGTextureParameter> DefaultWhite;
		std::optional<FRDGTextureParameter> DefaultShadowArray;
		std::optional<FRDGTextureParameter> EnvironmentIrradiance;
		std::optional<FRDGTextureParameter> EnvironmentPrefiltered;
		std::optional<FRDGTextureParameter> EnvironmentBrdfLut;
		std::optional<FRDGColorAttachmentParameter> SceneColorOutput;
		std::optional<FRDGManagedTextureParameter> SceneDepthGraphicsToGraphics;
		std::optional<FRDGManagedTextureParameter> SceneDepthGraphicsToDepth;
		std::optional<FRDGManagedTextureParameter> SceneDepthDepthToGraphics;
		std::optional<FRDGManagedTextureParameter> SceneDepthDepthToDepth;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FVolumetricCloudSpatialPassResources,
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGTextureParameter> SceneDepthCompute;
		std::optional<FRDGTextureParameter> CloudBaseDensity;
		std::optional<FRDGTextureParameter> CloudDetailDensity;
		std::optional<FRDGTextureParameter> CloudWeather;
		std::optional<FRDGTextureParameter> CloudBaseDensityCompute;
		std::optional<FRDGTextureParameter> CloudDetailDensityCompute;
		std::optional<FRDGTextureParameter> CloudWeatherCompute;
		std::optional<FRDGManagedTextureParameter> CloudFragmentOutput;
		std::optional<FRDGTextureParameter> CloudComputeOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FVolumetricCloudCompositePassResources,
		std::optional<FRDGTextureParameter> SceneColor;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGTextureParameter> CloudBaseDensity;
		std::optional<FRDGTextureParameter> CloudDetailDensity;
		std::optional<FRDGTextureParameter> CloudWeather;
		std::optional<FRDGTextureParameter> CloudShadowFragment;
		std::optional<FRDGTextureParameter> CloudShadowCompute;
		std::optional<FRDGTextureParameter> CloudFragment;
		std::optional<FRDGTextureParameter> CloudCompute;
		std::optional<FRDGManagedTextureParameter> CloudCompositeOutput;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FSceneColorPassResources,
		std::optional<FRDGManagedTextureParameter> SceneColorManaged;
		std::optional<FRDGManagedTextureParameter> SceneDepthManaged;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FPostProcessPassResources,
		std::optional<FRDGColorAttachmentParameter> OutputPresent;
		std::optional<FRDGColorAttachmentParameter> OutputOffscreen;
		std::optional<FRDGColorAttachmentParameter> OutputForEditor;
		std::optional<FRDGTextureParameter> SceneColor;
		std::optional<FRDGTextureParameter> CloudComposite;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGColorAttachmentParameter> GBufferDebugOutput;
		std::array<std::optional<FRDGTextureParameter>, 4> GBuffer;
		std::optional<FRDGTextureParameter> IsolatedDeferred;);
	DURIN_DECLARE_SCENE_PASS_RESOURCES(FEditorAssistancePassResources,
		std::optional<FRDGColorAttachmentParameter> EditorOutputPresent;
		std::optional<FRDGColorAttachmentParameter> EditorOutputOffscreen;
		std::optional<FRDGDepthStencilAttachmentParameter> EditorDepth;);

#undef DURIN_DECLARE_SCENE_PASS_RESOURCES

#define DURIN_DECLARE_SCENE_PASS_PARAMETERS(TypeName, ResourceType, Members) \
	struct TypeName final \
	{ \
		Members \
		ResourceType Resources; \
		static RENDERER_API auto GetRDGParametersMetadata() \
			-> const FRDGParametersMetadata*; \
	}

	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FDirectionalShadowPassParameters,
		FDirectionalShadowPassResources,
		TRDGValueWrite<FDirectionalShadowPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FAmbientOcclusionPassParameters,
		FAmbientOcclusionPassResources,
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueWrite<FGroundTruthAmbientOcclusionPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FVolumetricCloudShadowPassParameters,
		FVolumetricCloudShadowPassResources,
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueWrite<FVolumetricCloudShadowPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FDeferredDirectionalLightingPassParameters,
		FDeferredDirectionalLightingPassResources,
		TRDGValueRead<FDirectionalShadowPassResult> DirectionalShadow;
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueRead<FGroundTruthAmbientOcclusionPassResult> AmbientOcclusion;
		TRDGValueRead<FContactShadowVisibilityPassResult> ContactShadow;
		TRDGValueRead<FVolumetricCloudShadowPassResult> CloudShadow;
		TRDGValueWrite<FIsolatedDeferredPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FBaseScenePassParameters,
		FBaseScenePassResources,
		TRDGValueRead<FIsolatedDeferredPassResult> DeferredLighting;
		TRDGValueWrite<FSceneColorPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FVolumetricCloudSpatialPassParameters,
		FVolumetricCloudSpatialPassResources,
		TRDGValueRead<FSceneColorPassResult> BaseScene;
		TRDGValueWrite<FVolumetricCloudSpatialPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FVolumetricCloudCompositePassParameters,
		FVolumetricCloudCompositePassResources,
		TRDGValueRead<FSceneColorPassResult> BaseScene;
		TRDGValueRead<FVolumetricCloudSpatialPassResult> Spatial;
		TRDGValueRead<FVolumetricCloudShadowPassResult> CloudShadow;
		TRDGValueWrite<FVolumetricCloudPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FSceneColorPassParameters,
		FSceneColorPassResources,
		TRDGValueRead<FSceneColorPassResult> BaseScene;
		TRDGValueRead<FVolumetricCloudPassResult> VolumetricCloud;
		TRDGValueWrite<FSceneColorPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FPostProcessPassParameters,
		FPostProcessPassResources,
		TRDGValueRead<FSceneColorPassResult> SceneColor;
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueRead<FIsolatedDeferredPassResult> DeferredLighting;
		TRDGValueWrite<FPostProcessPassResult> Completion;);
	DURIN_DECLARE_SCENE_PASS_PARAMETERS(FEditorAssistancePassParameters,
		FEditorAssistancePassResources,
		TRDGValueRead<FPostProcessPassResult> PostProcess;);

#undef DURIN_DECLARE_SCENE_PASS_PARAMETERS

	struct FGBufferPassParameters final
	{
		TRDGValueWrite<FGBufferPassResult> Completion;
		std::array<std::optional<FRDGColorAttachmentParameter>, 4> Colors;
		std::optional<FRDGDepthStencilAttachmentParameter> Depth;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FContactShadowGraphicsPassParameters final
	{
		TRDGValueRead<FDirectionalShadowPassResult> DirectionalShadow;
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueWrite<FContactShadowVisibilityPassResult> Completion;
		std::optional<FRDGTextureParameter> GBufferMaterial;
		std::optional<FRDGTextureParameter> GBufferNormals;
		std::optional<FRDGTextureParameter> GBufferSurface;
		std::optional<FRDGTextureParameter> GBufferEmissive;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGColorAttachmentParameter> Output;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FContactShadowComputePassParameters final
	{
		TRDGValueRead<FDirectionalShadowPassResult> DirectionalShadow;
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueWrite<FContactShadowVisibilityPassResult> Completion;
		std::optional<FRDGTextureParameter> GBufferMaterial;
		std::optional<FRDGTextureParameter> GBufferNormals;
		std::optional<FRDGTextureParameter> GBufferSurface;
		std::optional<FRDGTextureParameter> GBufferEmissive;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGTextureParameter> ContactVisibilityOutput;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FDirectionalShadowGraphOutput final
	{
		TRDGValueHandle<FDirectionalShadowPassResult> Completion;
		std::optional<FRDGTextureHandle> Shadow;
	};
	struct FGBufferGraphOutput final
	{
		TRDGValueHandle<FGBufferPassResult> Completion;
		std::array<std::optional<FRDGTextureHandle>, 4> Textures;
		FRDGTextureHandle Depth;
	};
	struct FAmbientOcclusionGraphOutput final
	{
		TRDGValueHandle<FGroundTruthAmbientOcclusionPassResult> Completion;
		std::array<std::optional<FRDGTextureHandle>, 4> Textures;
		EGroundTruthAmbientOcclusionQuality Quality =
			EGroundTruthAmbientOcclusionQuality::FullResolution;
	};
	struct FContactShadowGraphOutput final
	{
		TRDGValueHandle<FContactShadowVisibilityPassResult> Completion;
		std::optional<FRDGTextureHandle> Fragment;
		std::optional<FRDGTextureHandle> Compute;
	};
	struct FCloudShadowGraphOutput final
	{
		TRDGValueHandle<FVolumetricCloudShadowPassResult> Completion;
		std::optional<FRDGTextureHandle> Fragment;
		std::optional<FRDGTextureHandle> Compute;
	};
	struct FDeferredLightingGraphOutput final
	{
		TRDGValueHandle<FIsolatedDeferredPassResult> Completion;
		std::optional<FRDGTextureHandle> Isolated;
	};
	struct FBaseSceneGraphOutput final
	{
		TRDGValueHandle<FSceneColorPassResult> Completion;
		FRDGTextureHandle Color;
		FRDGTextureHandle Depth;
	};
	struct FCloudSpatialGraphOutput final
	{
		TRDGValueHandle<FVolumetricCloudSpatialPassResult> Completion;
		std::optional<FRDGTextureHandle> Fragment;
		std::optional<FRDGTextureHandle> Compute;
		std::optional<FRDGTextureHandle> Composite;
	};
	struct FCloudCompositeGraphOutput final
	{
		TRDGValueHandle<FVolumetricCloudPassResult> Completion;
		std::optional<FRDGTextureHandle> Composite;
	};
	struct FSceneColorGraphOutput final
	{
		TRDGValueHandle<FSceneColorPassResult> Completion;
		FRDGTextureHandle Color;
		FRDGTextureHandle Depth;
		std::optional<FRDGTextureHandle> CloudComposite;
	};
	struct FPostProcessGraphOutput final
	{
		TRDGValueHandle<FPostProcessPassResult> Completion;
		FRDGTextureHandle Output;
	};

	struct FDirectionalShadowGraphInputs final
	{
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		FDirectionalShadowRecordInputs Record;
		std::optional<FRDGTextureHandle> Shadow;
	};
	struct FGBufferGraphInputs final
	{
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		FGBufferRecordInputs Record;
		FRDGTextureHandle Depth;
		const FSceneViewRenderOptions& Options;
		uint32 Width;
		uint32 Height;
		bool bEnabled;
		bool bNeedsGBuffer;
		bool bWantsIsolatedDeferred;
	};
	struct FAmbientOcclusionGraphInputs final
	{
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
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
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		FContactShadowVisibilityRecordInputs Record;
		const FSceneViewRenderOptions& Options;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		const FGBufferGraphOutput& GBuffer;
		FContactShadowVisibilityRenderer::FRouteDecision Route;
		ESceneRenderRoute GraphRoute;
		uint32 Width;
		uint32 Height;
		bool bProductionDeferred;
	};
	struct FCloudShadowGraphInputs final
	{
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		FVolumetricCloudShadowRecordInputs Record;
		const FGBufferGraphOutput& GBuffer;
		FRDGTextureHandle SceneDepth;
		std::optional<FRDGTextureHandle> BaseDensity;
		std::optional<FRDGTextureHandle> DetailDensity;
		std::optional<FRDGTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		FVolumetricCloudShadowRenderer::ERoute Route;
		ESceneRenderRoute GraphRoute;
		uint32 Width;
		uint32 Height;
		bool bProductionDeferred;
	};
	struct FDeferredLightingGraphInputs final
	{
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		const FSceneView& View;
		const FSceneViewRenderOptions& Options;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		const FGBufferGraphOutput& GBuffer;
		const FAmbientOcclusionGraphOutput& AmbientOcclusion;
		const FContactShadowGraphOutput& ContactShadow;
		const FCloudShadowGraphOutput& CloudShadow;
		std::optional<FRDGTextureHandle> DefaultWhite;
		std::optional<FRDGTextureHandle> DefaultShadowArray;
		std::optional<FRDGTextureHandle> EnvironmentIrradiance;
		std::optional<FRDGTextureHandle> EnvironmentPrefiltered;
		std::optional<FRDGTextureHandle> EnvironmentBrdfLut;
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
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		FSceneGeometryRecordInputs Record;
		const FDeferredLightingGraphOutput& Deferred;
		FRDGTextureHandle SceneColor;
		FRDGTextureHandle SceneDepth;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		std::optional<FRDGTextureHandle> DefaultWhite;
		std::optional<FRDGTextureHandle> DefaultShadowArray;
		std::optional<FRDGTextureHandle> EnvironmentIrradiance;
		std::optional<FRDGTextureHandle> EnvironmentPrefiltered;
		std::optional<FRDGTextureHandle> EnvironmentBrdfLut;
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
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		FVolumetricCloudRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		std::optional<FRDGTextureHandle> BaseDensity;
		std::optional<FRDGTextureHandle> DetailDensity;
		std::optional<FRDGTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		FVolumetricCloudRenderer::ERoute Route;
		ESceneRenderRoute GraphRoute;
		FIntPoint Extent;
		uint32 Width;
		uint32 Height;
		bool bComposite;
	};
	struct FCloudCompositeGraphInputs final
	{
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		FVolumetricCloudRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		const FCloudSpatialGraphOutput& Spatial;
		const FCloudShadowGraphOutput& CloudShadow;
		std::optional<FRDGTextureHandle> BaseDensity;
		std::optional<FRDGTextureHandle> DetailDensity;
		std::optional<FRDGTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		bool bEnabled;
	};
	struct FSceneColorGraphInputs final
	{
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		FSceneGeometryRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		const FCloudCompositeGraphOutput& VolumetricCloud;
		FSceneColorPassResult& Publication;
		bool bRequiresDeferredOpaque;
		bool bVolumetricCloudComposite;
	};
	struct FPostProcessGraphInputs final
	{
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		const FSceneView& RecordView;
		const FSceneView& View;
		const FSceneViewRenderOptions& Options;
		const FSceneColorGraphOutput& SceneColor;
		const FGBufferGraphOutput& GBuffer;
		const FDeferredLightingGraphOutput& Deferred;
		FRDGTextureHandle Output;
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
		FRDGBuilder& Graph;
		FSceneRenderGraphServices& Services;
		const FSceneView& View;
		const RendererEditorAssistance::FPrepared& Prepared;
		const FPostProcessGraphOutput& PostProcess;
		FRDGTextureHandle SceneDepth;
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
		bool, "Scene.EditorAssistance", FEditorAssistanceGraphInputs, void);

#undef DURIN_DECLARE_SCENE_GRAPH_CONTRIBUTOR

	template <typename TContributor, CRDGParameters TParameters,
		typename TCallback>
	[[nodiscard]] auto AddSceneRenderFeaturePass(
		FRDGBuilder& Graph,
		ERDGPassType Type,
		TRDGParametersRef<TParameters>&& Parameters,
		TCallback&& Callback) -> FRDGPassHandle
	{
		return Graph.AddPass(TContributor::Name, Type,
			std::move(Parameters), std::forward<TCallback>(Callback));
	}
} // namespace Durin
