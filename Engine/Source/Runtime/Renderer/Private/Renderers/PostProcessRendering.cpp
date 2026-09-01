#include "Renderers/PostProcessRendering.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	#define DURIN_RESOURCE(Field, Wrapper, Kind, Use, Access, ...) \
		MakeRDGResourceParameterMemberMetadata<FParameters, \
			decltype(FParameters::Field), Wrapper>(#Field, offsetof(FParameters, Field), \
				Kind, ERDGResourceKind::Texture, \
				ERDGParameterRangeKind::TextureSubresource, Use, Access \
				__VA_OPT__(,) __VA_ARGS__)
	#define DURIN_TEXTURE(Field) DURIN_RESOURCE(Field, FRDGTextureParameter, \
		ERDGParameterMemberKind::Texture, ERDGUse::Read, \
		ERHIAccess::GraphicsShaderRead)
	#define DURIN_COLOR(Field, Result) DURIN_RESOURCE(Field, \
		FRDGColorAttachmentParameter, ERDGParameterMemberKind::ManagedColorAttachment, \
		ERDGUse::ReadWrite, ERHIAccess::ColorAttachmentReadWrite, true, \
		ERHIRenderTargetLoadAction::Clear, ERHIRenderTargetStoreAction::Store, true, \
		Result)

	auto FPostProcessPassResources::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FPostProcessPassResources;
		static const std::array Members = {
			DURIN_COLOR(OutputPresent, ERHIAccess::Present),
			DURIN_COLOR(OutputOffscreen, ERHIAccess::GraphicsShaderRead),
			DURIN_COLOR(OutputForEditor, ERHIAccess::ColorAttachmentReadWrite),
			DURIN_TEXTURE(SceneColor), DURIN_TEXTURE(CloudComposite),
			DURIN_TEXTURE(SceneDepth),
			DURIN_COLOR(GBufferDebugOutput, ERHIAccess::GraphicsShaderRead),
			DURIN_TEXTURE(GBuffer), DURIN_TEXTURE(IsolatedDeferred)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FPostProcessPassResources", Members);
		return &Metadata;
	}

	auto FPostProcessPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FPostProcessPassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::SceneColor), FSceneColorPassResult>(
					"SceneColor", offsetof(FParameters, SceneColor)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferCompletion), FGBufferPassResult>(
					"GBufferCompletion", offsetof(FParameters, GBufferCompletion)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::DeferredLighting), FIsolatedDeferredPassResult>(
					"DeferredLighting", offsetof(FParameters, DeferredLighting)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::Completion), FPostProcessPassResult>(
					"Completion", offsetof(FParameters, Completion)),
			MakeRDGNestedParameterMemberMetadata<FParameters,
				decltype(FParameters::Resources)>("Resources",
					offsetof(FParameters, Resources),
					FPostProcessPassResources::GetRDGParametersMetadata())};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FPostProcessPassParameters", Members);
		return &Metadata;
	}

	#undef DURIN_COLOR
	#undef DURIN_TEXTURE
	#undef DURIN_RESOURCE

	namespace
	{
		struct FPostProcessRecorder final
		{
			FGBufferDebugRenderer& GBufferDebugRenderer;
			FPostProcessRenderer& PostProcessRenderer;
			FSceneRenderTelemetry& Telemetry;

			auto RenderPostProcess_RenderThread(FRHICommandListImmediate&,
				const FSceneView&, const FSceneView&, FRHITexture*, bool,
				const FSceneViewRenderOptions&,
				const FPostProcessRenderer::FSceneTargets&,
				const FGBufferRenderer::FTargets*,
				const FGBufferDebugRenderer::FTargets*, FRHITexture*, FRHITexture*,
				bool) -> FPostProcessPassResult;
		};

		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}
	} // namespace

	auto FPostProcessRendering::AddPasses(
		const FPostProcessFeatureInputs& Inputs) -> FPostProcessGraphOutput
	{
		auto& Graph = Inputs.Graph;
		FPostProcessRecorder Recorder{
			Inputs.GBufferDebug, Inputs.Renderer, Inputs.Telemetry};
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
			Graph.AddPass(Name, ERDGPassType::Graphics, std::move(Parameters),
			[Recorder, &Publication = Inputs.Publication,
				RecordView = &RecordView, &View, bGBufferDebug, &Options,
				bPresentOutput, bHasEditorAssistance](
				FRHICommandListImmediate& Commands,
				const FPostProcessPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) mutable {
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
				PostProcessResult = Recorder.RenderPostProcess_RenderThread(
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

	auto FPostProcessRecorder::RenderPostProcess_RenderThread(
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
