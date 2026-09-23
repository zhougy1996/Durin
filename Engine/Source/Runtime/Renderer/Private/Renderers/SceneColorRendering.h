#pragma once

#include "Renderers/BaseSceneRendering.h"
#include "Renderers/VolumetricCloudRendering.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	class FStaticMeshRenderer;
	struct FSceneRenderTelemetry;

	struct FSceneColorPassResources final
	{
		std::optional<FRDGManagedTextureParameter> SceneColorManaged;
		std::optional<FRDGManagedTextureParameter> SceneDepthManaged;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FSceneColorPassParameters final
	{
		TRDGValueRead<FSceneColorPassResult> BaseScene;
		std::optional<TRDGValueRead<FVolumetricCloudPassResult>> VolumetricCloud;
		TRDGValueWrite<FSceneColorPassResult> Completion;
		FSceneColorPassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FSceneColorGraphOutput final
	{
		TRDGValueHandle<FSceneColorPassResult> Completion;
		FRDGTextureHandle Color;
		FRDGTextureHandle Depth;
		std::optional<FRDGTextureHandle> CloudComposite;
	};

	struct FSceneColorFeatureInputs final
	{
		FSceneGeometryRecordInputs Record;
		const FBaseSceneGraphOutput& BaseScene;
		const FCloudCompositeGraphOutput& VolumetricCloud;
		FStaticMeshRenderer& StaticMeshes;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		FSceneColorPassResult& Publication;
		const FSceneFeatureDecision& DeferredFeature;
		const FSceneFrameFeaturePlan::FCloudSpatial& CloudFeature;
	};

	inline constexpr std::string_view SceneColorPassName = "Scene.Color";
	auto AddSceneColorPasses(FRDGBuilder& Graph, const FSceneColorFeatureInputs& Inputs)
		-> FSceneColorGraphOutput;
} // namespace Durin
