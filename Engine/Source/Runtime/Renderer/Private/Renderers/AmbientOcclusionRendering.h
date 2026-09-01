#pragma once

#include "Renderers/GBufferRendering.h"
#include "RDG.h"

namespace Durin
{
	class FGroundTruthAmbientOcclusionRenderer;
	class FRendererRDGAllocator;
	struct FSceneRenderTelemetry;
	struct FSceneView;

	struct FAmbientOcclusionPassResources final
	{
		std::array<std::optional<FRDGTextureParameter>, 4> GBuffer;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::array<std::optional<FRDGManagedTextureParameter>, 4>
			AmbientOcclusionManaged;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FAmbientOcclusionPassParameters final
	{
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueWrite<FGroundTruthAmbientOcclusionPassResult> Completion;
		FAmbientOcclusionPassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FAmbientOcclusionGraphOutput final
	{
		TRDGValueHandle<FGroundTruthAmbientOcclusionPassResult> Completion;
		std::array<std::optional<FRDGTextureHandle>, 4> Textures;
		EGroundTruthAmbientOcclusionQuality Quality =
			EGroundTruthAmbientOcclusionQuality::FullResolution;
	};

	struct FAmbientOcclusionFeatureInputs final
	{
		FRDGBuilder& Graph;
		const FSceneView& View;
		const FSceneViewRenderOptions& Options;
		const FGBufferGraphOutput& GBuffer;
		FRendererRDGAllocator& Allocator;
		FGroundTruthAmbientOcclusionRenderer& Renderer;
		FSceneRenderTelemetry& Telemetry;
		uint32 Width;
		uint32 Height;
		const FSceneFrameFeaturePlan::FAmbientOcclusion& Feature;
	};

	struct FAmbientOcclusionRendering final
	{
		using Result = FGroundTruthAmbientOcclusionPassResult;
		static constexpr std::string_view Name = "Scene.AmbientOcclusion";
		static auto AddPasses(const FAmbientOcclusionFeatureInputs& Inputs)
			-> FAmbientOcclusionGraphOutput;
	};
} // namespace Durin
