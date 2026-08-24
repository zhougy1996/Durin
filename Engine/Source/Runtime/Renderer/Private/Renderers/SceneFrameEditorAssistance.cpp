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
		FSceneFrameGraphContributorContext& Context,
		const FSceneView& RecordView) -> void
	{
		if (!Context.bHasEditorAssistance) return;
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		auto* OutputTarget = Context.OutputTarget;
		const bool bPresentOutput = Context.bPresentOutput;
		const auto& PreparedEditorAssistance =
			Context.EditorAssistance;
		auto& GraphResources = Context.Composition.Resources;
		auto& Channels = Context.Composition.Channels;
		auto& PostProcessValue = Channels.PostProcess;
			auto& FinalOutputValue = Channels.FinalOutput;
			const auto EditorAssistancePass =
				AddSceneFrameFeaturePass<FEditorAssistanceGraphContributor>(
					Graph, ERenderGraphPassType::Graphics,
				[&Services, &Channels, RecordView = &RecordView, &GraphResources,
					&PreparedEditorAssistance, bPresentOutput](
					FRHICommandListImmediate& Commands,
					const FRenderGraphPassResources& Resources) {
					if (Channels.PostProcess.Result.Result != ERenderViewResult::Success) return;
					Channels.PostProcess.Result.bEditorAssistance =
						Services.Recorders.RenderEditorAssistance_RenderThread(
							Commands, *RecordView,
							Resources.GetTexture(GraphResources.Output),
							Resources.GetTexture(GraphResources.SceneDepth),
							bPresentOutput, PreparedEditorAssistance);
				});
			Graph.UseToken(EditorAssistancePass, PostProcessValue.Handle,
				ERenderGraphUse::Read);
			Graph.UseToken(EditorAssistancePass, FinalOutputValue.Handle,
				ERenderGraphUse::Write);
			Graph.UseManagedColorAttachment(EditorAssistancePass,
				GraphResources.Output,
				{GetTextureAspects(OutputTarget->GetFormat()), 0,
					OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()},
				ERHIRenderTargetLoadAction::Load,
				ERHIRenderTargetStoreAction::Store,
				bPresentOutput ? ERHIAccess::Present
								 : ERHIAccess::GraphicsShaderRead);
			Graph.UseManagedDepthStencilAttachment(EditorAssistancePass,
				GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Load,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::DepthStencilReadWrite);
			Graph.MarkPassRoot(EditorAssistancePass,
				bPresentOutput ? "present" : "offscreen-output");
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
