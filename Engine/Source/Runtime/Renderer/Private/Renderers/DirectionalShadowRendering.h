#pragma once

#include "Renderers/SceneRenderGraphTypes.h"
#include "RDG.h"

namespace Durin
{
	class FDirectionalShadowRenderer;
	class FSkeletalMeshRenderer;
	class FStaticMeshRenderer;
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
		FRDGBuilder& Graph;
		const FPreparedDirectionalShadow* ShadowRecord;
		std::optional<FRDGTextureHandle> Shadow;
		FDirectionalShadowRenderer& Renderer;
		FStaticMeshRenderer& StaticMeshes;
		FSkeletalMeshRenderer& SkeletalMeshes;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
	};

	struct FDirectionalShadowRendering final
	{
		using Result = FDirectionalShadowPassResult;
		static constexpr std::string_view Name = "Scene.DirectionalShadow";
		static auto AddPasses(const FDirectionalShadowFeatureInputs& Inputs)
			-> FDirectionalShadowGraphOutput;
	};
} // namespace Durin
