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

	auto FPostProcessRendering::AddPasses(
		const FPostProcessGraphInputs& Inputs) -> FPostProcessGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto& RecordView = Inputs.RecordView;
		const auto& View = Inputs.View;
		auto* OutputTarget = Inputs.OutputTarget;
		const auto& Options = Inputs.Options;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bPresentOutput = Inputs.bPresentOutput;
		const bool bHasEditorAssistance =
			Inputs.EditorAssistanceFeature.IsEnabled();
		const bool bGBufferDebug = Inputs.GBufferDebugFeature.IsEnabled();
		std::optional<FRDGTextureHandle> GBufferDebug;
		const auto PostProcessCompletion = Graph.CreateValue<FPostProcessPassResult>(
			"Scene.PostProcessValue", "post-process-result");
		if (bGBufferDebug)
			GBufferDebug = Graph.CreateTexture(
				FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"GBufferDebugColor", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::GBufferDebug)},
				"Scene.GBuffer.Debug",
				ERHIAccess::GraphicsShaderRead);
		auto Parameters = Graph.AllocParameters<FPostProcessPassParameters>();
		Parameters->SceneColor = {.Value = Inputs.SceneColor.Completion};
		Parameters->GBufferCompletion = {.Value = Inputs.GBuffer.Completion};
		Parameters->DeferredLighting = {
			.Value = Inputs.Deferred.Completion};
		Parameters->Completion = {.Value = PostProcessCompletion};
		Parameters->Resources.SceneColor = {Inputs.SceneColor.Color,
			{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.SceneColor.CloudComposite)
			Parameters->Resources.CloudComposite = {
				*Inputs.SceneColor.CloudComposite,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (bGBufferDebug)
			Parameters->Resources.SceneDepth = {Inputs.SceneColor.Depth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
		const FRDGColorAttachmentParameter Output{
			Inputs.Output,
			{GetTextureAspects(OutputTarget->GetFormat()), 0,
				OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()}};
		if (bHasEditorAssistance)
			Parameters->Resources.OutputForEditor = Output;
		else if (bPresentOutput)
			Parameters->Resources.OutputPresent = Output;
		else
			Parameters->Resources.OutputOffscreen = Output;
		if (GBufferDebug)
			Parameters->Resources.GBufferDebugOutput = {
				*GBufferDebug,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.GBuffer.Textures[0] && bGBufferDebug)
			for (uint32 Index = 0; Index < Inputs.GBuffer.Textures.size(); ++Index)
				Parameters->Resources.GBuffer[Index] = {
					*Inputs.GBuffer.Textures[Index],
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.Deferred.Isolated)
			Parameters->Resources.IsolatedDeferred = {
				*Inputs.Deferred.Isolated,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		const auto PostProcessPass =
			AddSceneRenderFeaturePass<FPostProcessRendering>(
				Graph, ERDGPassType::Graphics, std::move(Parameters),
			[&Services, &Publication = Inputs.Publication,
				RecordView = &RecordView, &View, bGBufferDebug, &Options,
				bPresentOutput, bHasEditorAssistance](
				FRHICommandListImmediate& Commands,
				const FPostProcessPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				const auto& SceneColorResult = Resolver.ReadValue(
					PassParameters.SceneColor);
				auto& PostProcessResult = Resolver.WriteValue(
					PassParameters.Completion);
				if (!SceneColorResult.IsSuccess()) return;
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = Resolver.GetTexture(PassParameters.Resources.SceneColor),
					.Depth = bGBufferDebug
						? Resolver.GetTexture(PassParameters.Resources.SceneDepth)
						: nullptr};
				FRHITexture* SceneColorInput = SceneTargets.Color;
				if (SceneColorResult.bUsesVolumetricCloudComposite
					&& PassParameters.Resources.CloudComposite)
					SceneColorInput = Resolver.GetTexture(
						PassParameters.Resources.CloudComposite);
				std::optional<FGBufferDebugRenderer::FTargets> DebugTargets;
				if (PassParameters.Resources.GBufferDebugOutput)
					DebugTargets = {.Color = Resolver.GetColorAttachment(
						PassParameters.Resources.GBufferDebugOutput).Texture};
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (PassParameters.Resources.GBuffer[0] && bGBufferDebug)
					GBufferTargets = {
						.Material = Resolver.GetTexture(PassParameters.Resources.GBuffer[0]),
						.Normals = Resolver.GetTexture(PassParameters.Resources.GBuffer[1]),
						.Surface = Resolver.GetTexture(PassParameters.Resources.GBuffer[2]),
						.Emissive = Resolver.GetTexture(PassParameters.Resources.GBuffer[3])};
				FRHITexture* IsolatedDeferredOutput = nullptr;
				if (PassParameters.Resources.IsolatedDeferred
					&& Resolver.ReadValue(PassParameters.DeferredLighting).bOutputValid)
					IsolatedDeferredOutput = Resolver.GetTexture(
						PassParameters.Resources.IsolatedDeferred);
				FRHITexture* Output = Resolver.GetColorAttachment(
					PassParameters.Resources.OutputForEditor).Texture;
				if (Output == nullptr)
					Output = Resolver.GetColorAttachment(
						PassParameters.Resources.OutputPresent).Texture;
				if (Output == nullptr)
					Output = Resolver.GetColorAttachment(
						PassParameters.Resources.OutputOffscreen).Texture;
				PostProcessResult = Services.Recorders.RenderPostProcess_RenderThread(
					Commands, *RecordView, View,
					Output,
					bPresentOutput, Options, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					DebugTargets ? &*DebugTargets : nullptr,
					SceneColorInput, IsolatedDeferredOutput,
					bHasEditorAssistance);
				if (!bHasEditorAssistance)
					Publication = PostProcessResult;
			});
		if (!bHasEditorAssistance)
		{
			Graph.MarkPassRoot(PostProcessPass,
				bPresentOutput ? "present" : "offscreen-output");
		}
		return {.Completion = PostProcessCompletion, .Output = Inputs.Output};
	}

	auto FSceneRenderFeatureRecorders::RenderPostProcess_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FGBufferDebugRenderer::FTargets* GBufferDebugTargets,
		FRHITexture* SceneColor,
		FRHITexture* GroundTruthAmbientOcclusionDebugOutput,
		bool bEditorAssistanceFollows
	) -> FPostProcessPassResult
	{
		const uint32 Width = OutputTarget->GetSizeX();
		const uint32 Height = OutputTarget->GetSizeY();
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		FRHITexture* PostProcessInput = SceneColor;
		if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled
			&& GBufferTargets != nullptr)
		{
			if (GBufferDebugTargets != nullptr
				&& GBufferDebugRenderer.Render_RenderThread(
					CommandList,
					GBufferTargets->Material,
					GBufferTargets->Normals,
					GBufferTargets->Surface,
					GBufferTargets->Emissive,
					SceneTargets.Depth,
					GBufferDebugTargets->Color,
					RenderView,
					Options.GBufferDebugMode,
					Width,
					Height
				))
			{
				PostProcessInput = GBufferDebugTargets->Color;
				++Telemetry.View.GBuffer.GBufferDebugViews;
			}
			else
			{
				++Telemetry.View.GBuffer.GBufferDebugFailures;
			}
		}
		else if (GroundTruthAmbientOcclusionDebugOutput != nullptr)
		{
			PostProcessInput = GroundTruthAmbientOcclusionDebugOutput;
		}
		const FHDRSceneColorCaptureSink HDRCaptureSink =
			GetHDRSceneColorCaptureSink();
		if (HDRCaptureSink != nullptr)
		{
			HDRCaptureSink(CommandList, SceneColor, PostProcessInput);
		}

		FRHIRenderPassInfo PostProcessPassInfo{};
		PostProcessPassInfo.RenderTargetLayout = bEditorAssistanceFollows
			? RenderTargetLayouts::MakeScenePostProcessOutput()
			: RenderTargetLayouts::MakeFinalScenePostProcessOutput(ViewportOutput);
		PostProcessPassInfo.ColorRenderTargets[0] = OutputTarget;
		PostProcessPassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r,
			View.ClearColor.g,
			View.ClearColor.b,
			View.ClearColor.a
		);
		const FPostProcessTimingQuerySink PostProcessTimingSink =
			GetPostProcessTimingQuerySink();
		TScopedRendererGPUTimingQuery PostProcessTiming(
			CommandList, PostProcessTimingSink
		);
		CommandList.BeginRenderPass(
			PostProcessPassInfo,
			bPresentOutput ? "PostProcessPresentRenderPass" : "PostProcessOffscreenRenderPass"
		);
		PostProcessRenderer.Draw_RenderThread(
			CommandList,
			PostProcessInput,
			Width,
			Height,
			bPresentOutput,
			View.Settings.PostProcess.bEnableFXAA,
			bEditorAssistanceFollows,
			RenderView.Settings.PostProcess.ExposureEV
		);
		CommandList.EndRenderPass();
		PostProcessTiming.Commit();
		return {.Result = ERenderViewResult::Success};
	}
} // namespace Durin
