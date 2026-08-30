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

	auto FPostProcessGraphContributor::AddPasses(
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
		const bool bHasEditorAssistance = Inputs.bHasEditorAssistance;
		FSceneFrameTopology Topology;
		Topology.bGBufferDebug = Inputs.bGBufferDebug;
		struct {
			FRenderGraphTextureHandle SceneColor;
			FRenderGraphTextureHandle SceneDepth;
			FRenderGraphTextureHandle Output;
			std::array<std::optional<FRenderGraphTextureHandle>, 4> GBuffer;
			std::optional<FRenderGraphTextureHandle> VolumetricCloudComposite;
			std::optional<FRenderGraphTextureHandle> IsolatedDeferred;
			std::optional<FRenderGraphTextureHandle> GBufferDebug;
		} GraphResources;
		GraphResources.SceneColor = Inputs.SceneColor.Color;
		GraphResources.SceneDepth = Inputs.SceneColor.Depth;
		GraphResources.VolumetricCloudComposite = Inputs.SceneColor.CloudComposite;
		GraphResources.GBuffer = Inputs.GBuffer.Textures;
		GraphResources.IsolatedDeferred = Inputs.Deferred.Isolated;
		GraphResources.Output = Inputs.Output;
		struct {
			TSceneFrameGraphValue<FSceneColorPassResult> SceneColor;
			TSceneFrameGraphValue<FGBufferPassResult> GBuffer;
			TSceneFrameGraphValue<FIsolatedDeferredPassResult>
				DeferredDirectionalLighting;
			TSceneFrameGraphValue<FPostProcessPassResult> PostProcess;
			FRenderGraphTokenHandle OutputCompletion;
		} Channels;
		Channels.SceneColor.Handle = Inputs.SceneColor.Completion;
		Channels.GBuffer.Handle = Inputs.GBuffer.Completion;
		Channels.DeferredDirectionalLighting.Handle = Inputs.Deferred.Completion;
		Channels.PostProcess.Handle = Graph.CreateValue<FPostProcessPassResult>(
			"Scene.PostProcessValue", "post-process-result");
		Channels.OutputCompletion = Graph.CreateToken("Scene.OutputCompletion");
		if (Topology.bGBufferDebug)
			GraphResources.GBufferDebug = Graph.CreateTexture(
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
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
		Parameters->SceneColor = {.Value = Channels.SceneColor.Handle};
		Parameters->GBufferCompletion = {.Value = Channels.GBuffer.Handle};
		Parameters->DeferredLighting = {
			.Value = Channels.DeferredDirectionalLighting.Handle};
		Parameters->Completion = {.Value = Channels.PostProcess.Handle};
		Parameters->Resources.SceneColor = {GraphResources.SceneColor,
			{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudComposite)
			Parameters->Resources.CloudComposite = {
				*GraphResources.VolumetricCloudComposite,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Topology.bGBufferDebug)
			Parameters->Resources.SceneDepth = {GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
		const FRenderGraphColorAttachmentParameter Output{
			GraphResources.Output,
			{GetTextureAspects(OutputTarget->GetFormat()), 0,
				OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()}};
		if (bHasEditorAssistance)
			Parameters->Resources.OutputForEditor = Output;
		else if (bPresentOutput)
			Parameters->Resources.OutputPresent = Output;
		else
			Parameters->Resources.OutputOffscreen = Output;
		if (GraphResources.GBufferDebug)
			Parameters->Resources.GBufferDebugOutput = {
				*GraphResources.GBufferDebug,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.GBuffer[0] && Topology.bGBufferDebug)
			for (uint32 Index = 0; Index < GraphResources.GBuffer.size(); ++Index)
				Parameters->Resources.GBuffer[Index] = {
					*GraphResources.GBuffer[Index],
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.IsolatedDeferred)
			Parameters->Resources.IsolatedDeferred = {
				*GraphResources.IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (!bHasEditorAssistance)
			Parameters->OutputCompletion = FRenderGraphTokenParameter{
				Channels.OutputCompletion};
		const auto PostProcessPass =
			AddSceneFrameFeaturePass<FPostProcessGraphContributor>(
				Graph, ERenderGraphPassType::Graphics, std::move(Parameters),
			[&Services, &Publication = Inputs.Publication,
				RecordView = &RecordView, &View, Topology, &Options,
				bPresentOutput, bHasEditorAssistance](
				FRHICommandListImmediate& Commands,
				const FPostProcessPassParameters& PassParameters,
				const FRenderGraphParameterResolver& Resolver) {
				const auto& SceneColorResult = Resolver.ReadValue(
					PassParameters.SceneColor);
				auto& PostProcessResult = Resolver.WriteValue(
					PassParameters.Completion);
				if (!SceneColorResult.IsSuccess()) return;
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = Resolver.GetTexture(PassParameters.Resources.SceneColor),
					.Depth = Topology.bGBufferDebug
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
				if (PassParameters.Resources.GBuffer[0] && Topology.bGBufferDebug)
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
		return {.Completion = Channels.PostProcess.Handle,
			.Output = GraphResources.Output,
			.OutputCompletion = Channels.OutputCompletion};
	}

	auto FSceneFrameFeatureRecorders::RenderPostProcess_RenderThread(
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
