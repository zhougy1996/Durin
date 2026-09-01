#pragma once

#include "Renderers/GBufferRendering.h"
#include "RDG.h"

namespace Durin
{
	class FRendererRDGAllocator;
	class FVolumetricCloudRenderer;
	class FVolumetricCloudShadowRenderer;
	struct FPreparedLighting;
	struct FPreparedVolumetricCloud;
	struct FRendererQualificationPolicy;
	struct FBaseSceneGraphOutput;
	struct FSceneRenderTelemetry;

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

	struct FVolumetricCloudShadowPassResources final
	{
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGTextureParameter> SceneDepthCompute;
		std::optional<FRDGTextureParameter> CloudBaseDensity;
		std::optional<FRDGTextureParameter> CloudDetailDensity;
		std::optional<FRDGTextureParameter> CloudWeather;
		std::optional<FRDGTextureParameter> CloudBaseDensityCompute;
		std::optional<FRDGTextureParameter> CloudDetailDensityCompute;
		std::optional<FRDGTextureParameter> CloudWeatherCompute;
		std::optional<FRDGManagedTextureParameter> CloudShadowFragmentOutput;
		std::optional<FRDGTextureParameter> CloudShadowComputeOutput;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FVolumetricCloudShadowPassParameters final
	{
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueWrite<FVolumetricCloudShadowPassResult> Completion;
		FVolumetricCloudShadowPassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FVolumetricCloudSpatialPassResources final
	{
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGTextureParameter> SceneDepthCompute;
		std::optional<FRDGTextureParameter> CloudBaseDensity;
		std::optional<FRDGTextureParameter> CloudDetailDensity;
		std::optional<FRDGTextureParameter> CloudWeather;
		std::optional<FRDGTextureParameter> CloudBaseDensityCompute;
		std::optional<FRDGTextureParameter> CloudDetailDensityCompute;
		std::optional<FRDGTextureParameter> CloudWeatherCompute;
		std::optional<FRDGManagedTextureParameter> CloudFragmentOutput;
		std::optional<FRDGTextureParameter> CloudComputeOutput;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FVolumetricCloudSpatialPassParameters final
	{
		TRDGValueRead<FSceneColorPassResult> BaseScene;
		TRDGValueWrite<FVolumetricCloudSpatialPassResult> Completion;
		FVolumetricCloudSpatialPassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FVolumetricCloudCompositePassResources final
	{
		std::optional<FRDGTextureParameter> SceneColor;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGTextureParameter> CloudBaseDensity;
		std::optional<FRDGTextureParameter> CloudDetailDensity;
		std::optional<FRDGTextureParameter> CloudWeather;
		std::optional<FRDGTextureParameter> CloudShadowFragment;
		std::optional<FRDGTextureParameter> CloudShadowCompute;
		std::optional<FRDGTextureParameter> CloudFragment;
		std::optional<FRDGTextureParameter> CloudCompute;
		std::optional<FRDGManagedTextureParameter> CloudCompositeOutput;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FVolumetricCloudCompositePassParameters final
	{
		TRDGValueRead<FSceneColorPassResult> BaseScene;
		TRDGValueRead<FVolumetricCloudSpatialPassResult> Spatial;
		TRDGValueRead<FVolumetricCloudShadowPassResult> CloudShadow;
		TRDGValueWrite<FVolumetricCloudPassResult> Completion;
		FVolumetricCloudCompositePassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FCloudShadowGraphOutput final
	{
		TRDGValueHandle<FVolumetricCloudShadowPassResult> Completion;
		std::optional<FRDGTextureHandle> Fragment;
		std::optional<FRDGTextureHandle> Compute;
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

	struct FCloudShadowFeatureInputs final
	{
		FRDGBuilder& Graph;
		FVolumetricCloudShadowRecordInputs Record;
		const FGBufferGraphOutput& GBuffer;
		FRendererRDGAllocator& Allocator;
		FVolumetricCloudShadowRenderer& Renderer;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		const FRendererQualificationPolicy& Qualification;
		FRDGTextureHandle SceneDepth;
		std::optional<FRDGTextureHandle> BaseDensity;
		std::optional<FRDGTextureHandle> DetailDensity;
		std::optional<FRDGTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		const FSceneFrameFeaturePlan::FCloudShadow& Feature;
		const FSceneFeatureDecision& DeferredFeature;
		uint32 Width;
		uint32 Height;
	};

	struct FCloudSpatialFeatureInputs final
	{
		FRDGBuilder& Graph;
		FVolumetricCloudRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		FRendererRDGAllocator& Allocator;
		FVolumetricCloudRenderer& Renderer;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		FSceneViewTemporalContext& Temporal;
		FSceneViewState*& ViewState;
		const FRendererQualificationPolicy& Qualification;
		std::optional<FRDGTextureHandle> BaseDensity;
		std::optional<FRDGTextureHandle> DetailDensity;
		std::optional<FRDGTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		const FSceneFrameFeaturePlan::FCloudSpatial& Feature;
		uint32 Width;
		uint32 Height;
	};

	struct FCloudCompositeFeatureInputs final
	{
		FRDGBuilder& Graph;
		FVolumetricCloudRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		const FCloudSpatialGraphOutput& Spatial;
		const FCloudShadowGraphOutput& CloudShadow;
		FRendererRDGAllocator& Allocator;
		FVolumetricCloudRenderer& Renderer;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		FSceneViewTemporalContext& Temporal;
		FSceneViewState*& ViewState;
		std::optional<FRDGTextureHandle> BaseDensity;
		std::optional<FRDGTextureHandle> DetailDensity;
		std::optional<FRDGTextureHandle> Weather;
		FRHITexture* WeatherTexture;
		const FSceneFrameFeaturePlan::FCloudSpatial& Feature;
	};

	struct FVolumetricCloudShadowRendering final
	{
		using Result = FVolumetricCloudShadowPassResult;
		static constexpr std::string_view Name = "Scene.VolumetricCloudShadow";
		static auto AddPasses(const FCloudShadowFeatureInputs& Inputs)
			-> FCloudShadowGraphOutput;
	};

	struct FVolumetricCloudSpatialRendering final
	{
		using Result = FVolumetricCloudSpatialPassResult;
		static constexpr std::string_view Name = "Scene.VolumetricCloudSpatial";
		static auto AddPasses(const FCloudSpatialFeatureInputs& Inputs)
			-> FCloudSpatialGraphOutput;
	};

	struct FVolumetricCloudCompositeRendering final
	{
		using Result = FVolumetricCloudPassResult;
		static constexpr std::string_view Name = "Scene.VolumetricCloud";
		static auto AddPasses(const FCloudCompositeFeatureInputs& Inputs)
			-> FCloudCompositeGraphOutput;
	};
} // namespace Durin
