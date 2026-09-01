#include "Renderers/EditorAssistanceRendering.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	#define DURIN_ATTACHMENT(Field, Wrapper, Kind, Access, Result) \
		MakeRDGResourceParameterMemberMetadata<FParameters, \
			decltype(FParameters::Field), Wrapper>(#Field, offsetof(FParameters, Field), \
				Kind, ERDGResourceKind::Texture, \
				ERDGParameterRangeKind::TextureSubresource, ERDGUse::ReadWrite, Access, \
				false, ERHIRenderTargetLoadAction::Load, \
				ERHIRenderTargetStoreAction::Store, true, Result)
	auto FEditorAssistancePassResources::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FEditorAssistancePassResources;
		static const std::array Members = {
			DURIN_ATTACHMENT(EditorOutputPresent, FRDGColorAttachmentParameter,
				ERDGParameterMemberKind::ManagedColorAttachment,
				ERHIAccess::ColorAttachmentReadWrite, ERHIAccess::Present),
			DURIN_ATTACHMENT(EditorOutputOffscreen, FRDGColorAttachmentParameter,
				ERDGParameterMemberKind::ManagedColorAttachment,
				ERHIAccess::ColorAttachmentReadWrite, ERHIAccess::GraphicsShaderRead),
			DURIN_ATTACHMENT(EditorDepth, FRDGDepthStencilAttachmentParameter,
				ERDGParameterMemberKind::ManagedDepthStencilAttachment,
				ERHIAccess::DepthStencilReadWrite, ERHIAccess::DepthStencilReadWrite)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FEditorAssistancePassResources", Members);
		return &Metadata;
	}
	#undef DURIN_ATTACHMENT

	auto FEditorAssistancePassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FEditorAssistancePassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::PostProcess), FPostProcessPassResult>(
					"PostProcess", offsetof(FParameters, PostProcess)),
			MakeRDGNestedParameterMemberMetadata<FParameters,
				decltype(FParameters::Resources)>("Resources",
					offsetof(FParameters, Resources),
					FEditorAssistancePassResources::GetRDGParametersMetadata())};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FEditorAssistancePassParameters", Members);
		return &Metadata;
	}

	namespace
	{
		auto RecordEditorAssistance(FRHICommandListImmediate& CommandList,
			const FSceneView& RenderView, FRHITexture* OutputTarget,
			FRHITexture* DepthTarget, bool bPresentOutput,
			const RendererEditorAssistance::FPrepared& Prepared,
			FEditorAssistanceRenderer& Renderer) -> bool;

		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}
	} // namespace

	auto FEditorAssistanceRendering::AddPasses(
		const FEditorAssistanceFeatureInputs& Inputs) -> void
	{
		if (!Inputs.Feature.IsEnabled()) return;
		auto& Graph = Inputs.Graph;
		auto* Renderer = &Inputs.Renderer;
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
			Graph.AddPass(Name, ERDGPassType::Graphics, std::move(Parameters),
				[Renderer, &Publication = Inputs.Publication,
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
						RecordEditorAssistance(
							Commands, *RecordView,
							Output, Resolver.GetDepthStencilAttachment(
								PassParameters.Resources.EditorDepth).Texture,
							bPresentOutput, PreparedEditorAssistance, *Renderer);
				});
		Graph.MarkPassRoot(EditorAssistancePass,
			bPresentOutput ? "present" : "offscreen-output");
	}

	namespace
	{
	auto RecordEditorAssistance(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		FRHITexture* OutputTarget,
		FRHITexture* DepthTarget,
		bool bPresentOutput,
		const RendererEditorAssistance::FPrepared& Prepared,
		FEditorAssistanceRenderer& Renderer
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
		Renderer.Draw_RenderThread(
			CommandList,
			RenderView,
			Prepared
		);
		CommandList.EndRenderPass();
		return true;
	}
	} // namespace
} // namespace Durin
