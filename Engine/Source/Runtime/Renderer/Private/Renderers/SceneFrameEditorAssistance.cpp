#include "Renderers/SceneFrameGraphContributors.h"

#include "Renderers/SceneFrameFeatureRecorders.h"
#include "Renderers/SceneFrameGraphComposer.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}
	} // namespace

	auto FEditorAssistanceGraphContributor::AddPasses(
		const FEditorAssistanceGraphInputs& Inputs)
		-> FEditorAssistanceGraphOutput
	{
		if (!Inputs.bEnabled)
			return {.OutputCompletion = Inputs.PostProcess.OutputCompletion};
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto& RecordView = Inputs.View;
		auto* OutputTarget = Inputs.OutputTarget;
		const bool bPresentOutput = Inputs.bPresentOutput;
		const auto& PreparedEditorAssistance = Inputs.Prepared;
		struct {
			FRenderGraphTextureHandle Output;
			FRenderGraphTextureHandle SceneDepth;
		} GraphResources;
		GraphResources.Output = Inputs.PostProcess.Output;
		GraphResources.SceneDepth = Inputs.SceneDepth;
		struct {
			TSceneFrameGraphValue<FPostProcessPassResult> PostProcess;
			FRenderGraphTokenHandle OutputCompletion;
		} Channels;
		Channels.PostProcess.Handle = Inputs.PostProcess.Completion;
		Channels.OutputCompletion = Inputs.PostProcess.OutputCompletion;
		auto Parameters = Graph.AllocParameters<FEditorAssistancePassParameters>();
		Parameters->PostProcess = {.Value = Channels.PostProcess.Handle};
		Parameters->OutputCompletion = {.Token = Channels.OutputCompletion};
		const FRenderGraphColorAttachmentParameter Output{
			.Texture = GraphResources.Output,
			.Range = {GetTextureAspects(OutputTarget->GetFormat()), 0,
				OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()}};
		if (bPresentOutput)
			Parameters->Resources.EditorOutputPresent = Output;
		else
			Parameters->Resources.EditorOutputOffscreen = Output;
		Parameters->Resources.EditorDepth = {
			.Texture = GraphResources.SceneDepth,
			.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		const auto EditorAssistancePass =
			AddSceneFrameFeaturePass<FEditorAssistanceGraphContributor>(
				Graph, ERenderGraphPassType::Graphics, std::move(Parameters),
				[&Services, &Publication = Inputs.Publication,
					RecordView = &RecordView, &PreparedEditorAssistance,
					bPresentOutput](FRHICommandListImmediate& Commands,
					const FEditorAssistancePassParameters& PassParameters,
					const FRenderGraphParameterResolver& Resolver) {
					Publication = Resolver.ReadValue(
						PassParameters.PostProcess);
					if (Publication.Result
						!= ERenderViewResult::Success) return;
					FRHITexture* Output = Resolver.GetColorAttachment(
						PassParameters.Resources.EditorOutputPresent).Texture;
					if (Output == nullptr)
						Output = Resolver.GetColorAttachment(
							PassParameters.Resources.EditorOutputOffscreen).Texture;
					Publication.bEditorAssistance =
						Services.Recorders.RenderEditorAssistance_RenderThread(
							Commands, *RecordView,
							Output, Resolver.GetDepthStencilAttachment(
								PassParameters.Resources.EditorDepth).Texture,
							bPresentOutput, PreparedEditorAssistance);
				});
		Graph.MarkPassRoot(EditorAssistancePass,
			bPresentOutput ? "present" : "offscreen-output");
		return {.OutputCompletion = Channels.OutputCompletion};
	}

	auto FSceneFrameFeatureRecorders::RenderEditorAssistance_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		FRHITexture* OutputTarget,
		FRHITexture* DepthTarget,
		bool bPresentOutput,
		const RendererEditorAssistance::FPrepared& Prepared
	) -> bool
	{
		if (!Prepared.HasDrawableOperation()) return false;
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		FRHIRenderPassInfo EditorAssistancePassInfo{};
		EditorAssistancePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeEditorAssistanceOutput(ViewportOutput);
		EditorAssistancePassInfo.ColorRenderTargets[0] = OutputTarget;
		EditorAssistancePassInfo.DepthStencilRenderTarget =
			DepthTarget;
		CommandList.BeginRenderPass(
			EditorAssistancePassInfo,
			bPresentOutput ? "EditorAssistancePresentRenderPass" : "EditorAssistanceOffscreenRenderPass"
		);
		EditorAssistanceRenderer.Draw_RenderThread(
			CommandList,
			RenderView,
			Prepared
		);
		CommandList.EndRenderPass();
		return true;
	}
} // namespace Durin
