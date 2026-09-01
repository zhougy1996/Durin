#pragma once

#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/GBufferRendering.h"
#include "RDG.h"

namespace Durin
{
	class FRendererRDGAllocator;
	struct FDirectionalShadowGraphOutput;
	struct FPreparedDirectionalShadow;
	struct FSceneRenderTelemetry;
	struct FSceneView;

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

	struct FContactShadowGraphOutput final
	{
		TRDGValueHandle<FContactShadowVisibilityPassResult> Completion;
		std::optional<FRDGTextureHandle> Fragment;
		std::optional<FRDGTextureHandle> Compute;
	};

	struct FContactShadowFeatureInputs final
	{
		FRDGBuilder& Graph;
		const FSceneView& View;
		const FPreparedDirectionalShadow* Shadow;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		FRendererRDGAllocator& Allocator;
		FContactShadowVisibilityRenderer& Renderer;
		const FDirectionalShadowGraphOutput& DirectionalShadow;
		const FGBufferGraphOutput& GBuffer;
		const FSceneFrameFeaturePlan::FContactVisibility& Feature;
		uint32 Width;
		uint32 Height;
	};

	struct FContactShadowVisibilityRendering final
	{
		using Result = FContactShadowVisibilityPassResult;
		static constexpr std::string_view Name =
			"Scene.ContactShadowVisibility";
		static auto AddPasses(const FContactShadowFeatureInputs& Inputs)
			-> FContactShadowGraphOutput;
	};
} // namespace Durin
