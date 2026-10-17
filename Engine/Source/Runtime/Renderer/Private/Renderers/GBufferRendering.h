#pragma once

#include "CoreMinimal.h"

#include "Renderers/SceneRenderGraphTypes.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/MeshRendererShared.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	class FGBufferRenderer;
	class FStaticMeshRenderer;
	struct FPreparedReceiverGeometry;
	struct FSceneRenderTelemetry;
	struct FSceneView;

	struct FGBufferGPUCullingPlan final
	{
		static constexpr uint32 InvalidArgument = UINT32_MAX;
		static constexpr uint32 GroupedMember = UINT32_MAX - 1;
		static constexpr size_t MinimumGroupCandidates = 64;
		static constexpr size_t MaximumCandidates = 65'536;
		static constexpr size_t MaximumGeneratedBytes = 4 * 1024 * 1024;
		std::vector<FGBufferGPUCullingCandidate> Candidates;
		std::vector<FRHIDrawIndexedIndirectArguments> Arguments;
		std::vector<RendererPrivate::FStaticMeshTransformUniform> Transforms;
		std::vector<uint32> ArgumentByResolvedDraw;
		bool bDispatched = false;
	};

	struct FGBufferGPUCullingPassParameters final
	{
		FRDGBufferParameter Candidates;
		FRDGBufferParameter VisibleInstances;
		FRDGBufferParameter Arguments;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FGBufferGPUCullingCopyParameters final
	{
		TRDGValueRead<FGBufferPassResult> Completion;
		FRDGBufferParameter Arguments;
		FRDGTextureParameter Target;
		static RENDERER_API auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
	};
	struct FGBufferGPUCullingReadbackParameters final
	{
		FRDGTextureParameter Target;
		static RENDERER_API auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*;
	};

	struct FGPUCullingAdmissionDiagnostics final
	{
		size_t BelowThresholdCandidates = 0;
		size_t IncompatibleCandidates = 0;
		size_t PreparationUnavailableCandidates = 0;
	};

	RENDERER_API auto BuildGBufferGPUCullingPlan(
		const FPreparedStaticMeshView& Prepared,
		const FResolvedStaticMeshView& Resolved,
		const FSceneView& View,
		bool bRequested, bool bIndirectDrawSupported,
		size_t* OutOverflowCandidates = nullptr,
		size_t MaximumGeneratedBytes = FGBufferGPUCullingPlan::MaximumGeneratedBytes,
		FGPUCullingAdmissionDiagnostics* OutDiagnostics = nullptr)
		-> std::shared_ptr<FGBufferGPUCullingPlan>;

	struct FGBufferPassParameters final
	{
		TRDGValueWrite<FGBufferPassResult> Completion;
		std::array<std::optional<FRDGColorAttachmentParameter>, 4> Colors;
		std::optional<FRDGDepthStencilAttachmentParameter> Depth;
		std::optional<FRDGBufferParameter> VisibleInstances;
		std::optional<FRDGBufferParameter> InstanceTransforms;
		std::optional<FRDGBufferParameter> Arguments;

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
