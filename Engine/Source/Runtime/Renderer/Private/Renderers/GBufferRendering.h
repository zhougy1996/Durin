#pragma once

#include "Renderers/SceneRenderGraphTypes.h"
#include "RDG.h"

namespace Durin
{
	class FGBufferRenderer;
	class FSkeletalMeshRenderer;
	class FStaticMeshRenderer;
	struct FPreparedReceiverGeometry;
	struct FSceneRenderTelemetry;
	struct FSceneView;

	struct FGBufferPassParameters final
	{
		TRDGValueWrite<FGBufferPassResult> Completion;
		std::array<std::optional<FRDGColorAttachmentParameter>, 4> Colors;
		std::optional<FRDGDepthStencilAttachmentParameter> Depth;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FGBufferGraphOutput final
	{
		TRDGValueHandle<FGBufferPassResult> Completion;
		std::array<std::optional<FRDGTextureHandle>, 4> Textures;
		FRDGTextureHandle Depth;
	};

	struct FGBufferFeatureInputs final
	{
		FRDGBuilder& Graph;
		const FSceneView& View;
		const FPreparedReceiverGeometry& Receiver;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		FGBufferRenderer& Renderer;
		FStaticMeshRenderer& StaticMeshes;
		FSkeletalMeshRenderer& SkeletalMeshes;
		FRDGTextureHandle Depth;
		const FSceneViewRenderOptions& Options;
		uint32 Width;
		uint32 Height;
		const FSceneFeatureDecision& Feature;
		const FSceneFeatureDecision& DeferredFeature;
	};

	struct FGBufferRendering final
	{
		using Result = FGBufferPassResult;
		static constexpr std::string_view Name = "Scene.GBuffer";
		static auto AddPasses(const FGBufferFeatureInputs& Inputs)
			-> FGBufferGraphOutput;
	};
} // namespace Durin
