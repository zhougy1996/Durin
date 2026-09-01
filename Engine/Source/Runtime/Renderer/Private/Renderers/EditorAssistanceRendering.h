#pragma once

#include "Renderers/PostProcessRendering.h"
#include "RDG.h"

namespace Durin
{
	class FEditorAssistanceRenderer;

	struct FEditorAssistancePassResources final
	{
		std::optional<FRDGColorAttachmentParameter> EditorOutputPresent;
		std::optional<FRDGColorAttachmentParameter> EditorOutputOffscreen;
		std::optional<FRDGDepthStencilAttachmentParameter> EditorDepth;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FEditorAssistancePassParameters final
	{
		TRDGValueRead<FPostProcessPassResult> PostProcess;
		FEditorAssistancePassResources Resources;

		static RENDERER_API auto GetRDGParametersMetadata()
			-> const FRDGParametersMetadata*;
	};

	struct FEditorAssistanceFeatureInputs final
	{
		FRDGBuilder& Graph;
		const FSceneView& View;
		const RendererEditorAssistance::FPrepared& Prepared;
		const FPostProcessGraphOutput& PostProcess;
		FEditorAssistanceRenderer& Renderer;
		FRDGTextureHandle SceneDepth;
		FRHITexture* OutputTarget;
		FPostProcessPassResult& Publication;
		const FSceneFeatureDecision& Feature;
		bool bPresentOutput;
	};

	struct FEditorAssistanceRendering final
	{
		using Result = bool;
		static constexpr std::string_view Name = "Scene.EditorAssistance";
		static auto AddPasses(const FEditorAssistanceFeatureInputs& Inputs)
			-> void;
	};
} // namespace Durin
