#include "Renderers/SceneRenderGraphContributors.h"

#include "Renderers/SceneRenderFeatureRecorders.h"
#include "Renderers/SceneRenderGraphComposer.h"
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
		const FEditorAssistanceGraphInputs& Inputs) -> void
	{
		if (!Inputs.bEnabled) return;
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto& RecordView = Inputs.View;
		auto* OutputTarget = Inputs.OutputTarget;
		const bool bPresentOutput = Inputs.bPresentOutput;
		const auto& PreparedEditorAssistance = Inputs.Prepared;
		auto Parameters = Graph.AllocParameters<FEditorAssistancePassParameters>();
		Parameters->PostProcess = {.Value = Inputs.PostProcess.Completion};
		const FRDGColorAttachmentParameter Output{
			.Texture = Inputs.PostProcess.Output,
			.Range = {GetTextureAspects(OutputTarget->GetFormat()), 0,
				OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()}};
		if (bPresentOutput)
			Parameters->Resources.EditorOutputPresent = Output;
		else
			Parameters->Resources.EditorOutputOffscreen = Output;
		Parameters->Resources.EditorDepth = {
			.Texture = Inputs.SceneDepth,
			.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		const auto EditorAssistancePass =
			AddSceneRenderFeaturePass<FEditorAssistanceGraphContributor>(
				Graph, ERDGPassType::Graphics, std::move(Parameters),
				[&Services, &Publication = Inputs.Publication,
					RecordView = &RecordView, &PreparedEditorAssistance,
					bPresentOutput](FRHICommandListImmediate& Commands,
					const FEditorAssistancePassParameters& PassParameters,
					const FRDGParameterResolver& Resolver) {
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
	}

	auto FSceneRenderFeatureRecorders::RenderEditorAssistance_RenderThread(
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
