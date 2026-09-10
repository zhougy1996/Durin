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

	// Half-resolution AO requires both reconstruction targets or neither.
	struct FAmbientOcclusionHalfResolutionTextures final
	{
		FRDGTextureHandle Selector;
		FRDGTextureHandle Resolved;
	};

	// Every requested AO route has Raw/Scratch; reconstruction is one optional set.
	struct FAmbientOcclusionTextureHandles final
	{
		FRDGTextureHandle Raw;
		FRDGTextureHandle Scratch;
		std::optional<FAmbientOcclusionHalfResolutionTextures> HalfResolution;
	};

	struct FAmbientOcclusionGraphOutput final
	{
		// Absence means the feature was not requested; no producer pass exists.
		std::optional<TRDGValueHandle<FGroundTruthAmbientOcclusionPassResult>> Completion;
		std::optional<FAmbientOcclusionTextureHandles> Textures;
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
