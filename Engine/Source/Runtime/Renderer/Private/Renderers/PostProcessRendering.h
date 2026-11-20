#pragma once

#include "CoreMinimal.h"

#include "Renderers/SceneRenderResults.h"

#include "Renderers/DeferredDirectionalLightingRendering.h"
#include "Renderers/SceneColorRendering.h"
#include "RDG/RDGParameters.h"

namespace Durin
{
	class FGBufferDebugRenderer;
	class FPostProcessRenderer;
	struct FPreparedPostProcess;
	struct FPreparedPostProcessMaterial;
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
		std::array<std::optional<FRDGTextureParameter>, 3> GBuffer;
		std::optional<FRDGTextureParameter> IsolatedDeferred;
		std::optional<FRDGTextureParameter> BasePassDebug;
		std::optional<FRDGTextureParameter> MaterialOutput;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FPostProcessPassParameters final
	{
		TRDGValueRead<FSceneColorPassResult> SceneColor;
		std::optional<TRDGValueRead<FIsolatedDeferredPassResult>> DeferredLighting;
		std::optional<TRDGValueRead<FGBufferPassResult>> BasePassDebugCompletion;
		std::optional<TRDGValueRead<bool>> MaterialCompletion;
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
		uint32 Width;
		uint32 Height;
		const FSceneFeatureDecision& GBufferDebugFeature;
		const FSceneFeatureDecision& EditorAssistanceFeature;
		bool bPresentOutput;
		FPreparedPostProcess&& Draw;
		std::shared_ptr<const FPreparedPostProcessMaterial> Material;
	};

	inline constexpr std::string_view PostProcessPassName = "Scene.PostProcess";
	auto AddPostProcessPasses(FRDGBuilder& Graph, const FPostProcessFeatureInputs& Inputs)
		-> FPostProcessGraphOutput;
} // namespace Durin
