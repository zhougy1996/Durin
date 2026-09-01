#pragma once

#include "Renderers/DeferredDirectionalLightingRendering.h"
#include "Renderers/SceneColorRendering.h"
#include "RDG.h"

namespace Durin
{
	class FGBufferDebugRenderer;
	class FPostProcessRenderer;
	struct FSceneRenderTelemetry;

	struct FPostProcessPassResources final
	{
		std::optional<FRDGColorAttachmentParameter> OutputPresent;
		std::optional<FRDGColorAttachmentParameter> OutputOffscreen;
		std::optional<FRDGColorAttachmentParameter> OutputForEditor;
		std::optional<FRDGTextureParameter> SceneColor;
		std::optional<FRDGTextureParameter> CloudComposite;
		std::optional<FRDGTextureParameter> SceneDepth;
		std::optional<FRDGColorAttachmentParameter> GBufferDebugOutput;
		std::array<std::optional<FRDGTextureParameter>, 4> GBuffer;
		std::optional<FRDGTextureParameter> IsolatedDeferred;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FPostProcessPassParameters final
	{
		TRDGValueRead<FSceneColorPassResult> SceneColor;
		TRDGValueRead<FGBufferPassResult> GBufferCompletion;
		TRDGValueRead<FIsolatedDeferredPassResult> DeferredLighting;
		TRDGValueWrite<FPostProcessPassResult> Completion;
		FPostProcessPassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FPostProcessGraphOutput final
	{
		TRDGValueHandle<FPostProcessPassResult> Completion;
		FRDGTextureHandle Output;
	};

	struct FPostProcessFeatureInputs final
	{
		FRDGBuilder& Graph;
		const FSceneView& RecordView;
		const FSceneView& View;
		const FSceneViewRenderOptions& Options;
		const FSceneColorGraphOutput& SceneColor;
		const FGBufferGraphOutput& GBuffer;
		const FDeferredLightingGraphOutput& Deferred;
		FGBufferDebugRenderer& GBufferDebug;
		FPostProcessRenderer& Renderer;
		FSceneRenderTelemetry& Telemetry;
		FRDGTextureHandle Output;
		FRHITexture* OutputTarget;
		FPostProcessPassResult& Publication;
		uint32 Width;
		uint32 Height;
		const FSceneFeatureDecision& GBufferDebugFeature;
		const FSceneFeatureDecision& EditorAssistanceFeature;
		bool bPresentOutput;
	};

	struct FPostProcessRendering final
	{
		using Result = FPostProcessPassResult;
		static constexpr std::string_view Name = "Scene.PostProcess";
		static auto AddPasses(const FPostProcessFeatureInputs& Inputs)
			-> FPostProcessGraphOutput;
	};
} // namespace Durin
