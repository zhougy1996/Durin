#pragma once

#include "Renderers/GBufferRendering.h"
#include "RDG/RDGParameters.h"

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
		std::optional<TRDGValueRead<FVolumetricCloudShadowPassResult>> CloudShadow;
		TRDGValueWrite<FVolumetricCloudPassResult> Completion;
		FVolumetricCloudCompositePassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FCloudShadowGraphOutput final
	{
		// Absence means the feature was not requested; no producer pass exists.
		std::optional<TRDGValueHandle<FVolumetricCloudShadowPassResult>> Completion;
		std::optional<FRDGTextureHandle> Fragment;
		std::optional<FRDGTextureHandle> Compute;
	};

	struct FCloudSpatialGraphOutput final
	{
		std::optional<TRDGValueHandle<FVolumetricCloudSpatialPassResult>> Completion;
		std::optional<FRDGTextureHandle> Fragment;
		std::optional<FRDGTextureHandle> Compute;
		std::optional<FRDGTextureHandle> Composite;
	};

	struct FCloudCompositeGraphOutput final
	{
		std::optional<TRDGValueHandle<FVolumetricCloudPassResult>> Completion;
		std::optional<FRDGTextureHandle> Composite;
	};

	// Authoring handles and physical range metadata shared by cloud stages.
	struct FCloudDensityInputs final
	{
		std::optional<FRDGTextureHandle> BaseDensity;
		std::optional<FRDGTextureHandle> DetailDensity;
		std::optional<FRDGTextureHandle> Weather;
		FRHITexture* WeatherTexture;
	};

	struct FCloudShadowFeatureInputs final
	{
		FVolumetricCloudShadowRecordInputs Record;
		const FGBufferGraphOutput& GBuffer;
		FRendererRDGAllocator& Allocator;
		FVolumetricCloudShadowRenderer& Renderer;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		const FRendererQualificationPolicy& Qualification;
		FRDGTextureHandle SceneDepth;
		FCloudDensityInputs Density;
		const FSceneFrameFeaturePlan::FCloudShadow& Feature;
		uint32 Width;
		uint32 Height;
	};

	struct FCloudSpatialFeatureInputs final
	{
		FVolumetricCloudRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		FRendererRDGAllocator& Allocator;
		FVolumetricCloudRenderer& Renderer;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		FSceneViewTemporalContext& Temporal;
		FSceneViewState*& ViewState;
		const FRendererQualificationPolicy& Qualification;
		FCloudDensityInputs Density;
		const FSceneFrameFeaturePlan::FCloudSpatial& Feature;
		uint32 Width;
		uint32 Height;
	};

	struct FCloudCompositeFeatureInputs final
	{
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
		FCloudDensityInputs Density;
		const FSceneFrameFeaturePlan::FCloudSpatial& Feature;
	};

	inline constexpr std::string_view VolumetricCloudShadowPassName = "Scene.VolumetricCloudShadow";
	auto AddVolumetricCloudShadowPasses(FRDGBuilder& Graph, const FCloudShadowFeatureInputs& Inputs)
		-> FCloudShadowGraphOutput;

	inline constexpr std::string_view VolumetricCloudSpatialPassName = "Scene.VolumetricCloudSpatial";
	auto AddVolumetricCloudSpatialPasses(FRDGBuilder& Graph, const FCloudSpatialFeatureInputs& Inputs)
		-> FCloudSpatialGraphOutput;

	inline constexpr std::string_view VolumetricCloudCompositePassName = "Scene.VolumetricCloud";
	auto AddVolumetricCloudCompositePasses(FRDGBuilder& Graph, const FCloudCompositeFeatureInputs& Inputs)
		-> FCloudCompositeGraphOutput;
} // namespace Durin
