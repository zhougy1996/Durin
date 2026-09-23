#pragma once

#include "Renderers/SceneRenderGraphTypes.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	class FGBufferRenderer;
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

	// The four GBuffer color attachments are declared and consumed as one set.
	struct FGBufferTextureHandles final
	{
		std::array<FRDGTextureHandle, 4> Colors;
	};

	struct FGBufferGraphOutput final
	{
		std::optional<TRDGValueHandle<FGBufferPassResult>> Completion;
		std::optional<FGBufferTextureHandles> Textures;
		FRDGTextureHandle Depth;
	};

	struct FGBufferFeatureInputs final
	{
		const FSceneView& View;
		const FPreparedReceiverGeometry& Receiver;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		FGBufferRenderer& Renderer;
		FStaticMeshRenderer& StaticMeshes;
		FRDGTextureHandle Depth;
		const FSceneViewRenderOptions& Options;
		uint32 Width;
		uint32 Height;
		const FSceneFeatureDecision& Feature;
		const FSceneFeatureDecision& DeferredFeature;
	};

	inline constexpr std::string_view GBufferPassName = "Scene.GBuffer";
	auto AddGBufferPasses(FRDGBuilder& Graph, const FGBufferFeatureInputs& Inputs)
		-> FGBufferGraphOutput;
} // namespace Durin
