#pragma once

#include "Renderers/SceneRenderGraphTypes.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	class FDirectionalShadowRenderer;
	struct FPreparedDirectionalShadow;
	struct FSceneRenderTelemetry;

	struct FDirectionalShadowPassResources final
	{
		std::optional<FRDGDepthStencilAttachmentParameter>
			DirectionalShadowOutput;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FDirectionalShadowPassParameters final
	{
		TRDGValueWrite<FDirectionalShadowPassResult> Completion;
		FDirectionalShadowPassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FDirectionalShadowGraphOutput final
	{
		TRDGValueHandle<FDirectionalShadowPassResult> Completion;
		std::optional<FRDGTextureHandle> Shadow;
	};

	struct FDirectionalShadowFeatureInputs final
	{
		const FPreparedDirectionalShadow* ShadowRecord;
		std::optional<FRDGTextureHandle> Shadow;
		FDirectionalShadowRenderer& Renderer;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
	};

	inline constexpr std::string_view DirectionalShadowPassName = "Scene.DirectionalShadow";
	auto AddDirectionalShadowPasses(FRDGBuilder& Graph, const FDirectionalShadowFeatureInputs& Inputs)
		-> FDirectionalShadowGraphOutput;
} // namespace Durin
