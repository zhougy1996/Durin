#pragma once

#include "CoreMinimal.h"
#include "Renderers/InstanceCulling.h"
#include "Renderers/MeshDrawCommandStats.h"

#include "Renderers/SceneRendererProfiling.h"

#include "Renderers/SceneRenderResults.h"

#include "Renderers/SceneRenderGraphTypes.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/MeshRendererShared.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	class FGBufferRenderer;
	class FGBufferDebugRenderer;
	class FStaticMeshRenderer;
	struct FPreparedReceiverGeometry;
	struct FSceneRenderTelemetry;
	struct FSceneView;

	struct FGBufferPassParameters final
	{
		TRDGValueWrite<FGBufferPassResult> Completion;
		std::array<std::optional<FRDGColorAttachmentParameter>, 5> Colors;
		std::optional<FRDGDepthStencilAttachmentParameter> Depth;
		FInstanceCullingDrawParams InstanceCulling;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	// The three GBuffer color attachments are declared and consumed as one set.
	struct FGBufferTextureHandles final
	{
		std::array<FRDGTextureHandle, 3> Colors;
		std::optional<FRDGTextureHandle> ModelData;
	};

	struct FGBufferGraphOutput final
	{
		std::optional<TRDGValueHandle<FGBufferPassResult>> Completion;
		std::optional<FGBufferTextureHandles> Textures;
		FRDGTextureHandle Depth;
		FRDGTextureHandle BasePassColor;
		std::optional<FRDGTextureHandle> BasePassDebug;
		std::optional<TRDGValueHandle<FGBufferPassResult>> BasePassDebugCompletion;
	};

	struct FGBufferFeatureInputs final
	{
		const FSceneView& View;
		const FPreparedReceiverGeometry& Receiver;
		FResolvedSceneResources& Resolved;
		FSceneRenderTelemetry& Telemetry;
		FInstanceCullingManager& CullingManager;
		FStaticMeshRenderer& StaticMeshes;
		RendererPrivate::FMeshDrawCommandStatsManager& MeshDrawCommandStats;
		FRDGTextureHandle Depth;
		FRDGTextureHandle SceneColor;
		FGBufferDebugRenderer& DebugRenderer;
		const FSceneViewRenderOptions& Options;
		EGPUCullingFailure Failure = EGPUCullingFailure::None;
		uint32 Width;
		uint32 Height;
		const FSceneFrameFeaturePlan::FGBuffer& Feature;
		const FSceneFeatureDecision& DeferredFeature;
	};

	inline constexpr std::string_view GBufferPassName = "Scene.GBuffer";
	auto AddGBufferPasses(FRDGBuilder& Graph, const FGBufferFeatureInputs& Inputs)
		-> FGBufferGraphOutput;
} // namespace Durin
