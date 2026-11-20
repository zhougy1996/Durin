#include "Renderers/PostProcessRendering.h"
#include "RDG/RDG.h"
#include "Renderers/SceneTextureGroupParameters.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/GBufferDebugRenderer.h"
#include "Renderers/PostProcessRenderer.h"
#include "Renderers/MaterialSceneTextures.h"
#include "Materials/MaterialShaderMap.h"
#include "Renderers/RendererRDGAllocator.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"

namespace Durin
{
	#define DURIN_TEXTURE(Field) \
		MakeRDGTextureReadMetadata<FParameters, decltype(FParameters::Field)>( \
			#Field, offsetof(FParameters, Field))
	#define DURIN_COLOR(Field, Result) \
		MakeRDGAttachmentMetadata<FParameters, decltype(FParameters::Field)>( \
			#Field, offsetof(FParameters, Field), ERHIRenderTargetLoadAction::Clear, \
			ERHIRenderTargetStoreAction::Store, Result)

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
			DURIN_TEXTURE(GBuffer), DURIN_TEXTURE(IsolatedDeferred), DURIN_TEXTURE(BasePassDebug), DURIN_TEXTURE(MaterialOutput)
		};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FPostProcessPassResources", Members);
		return &Metadata;
	}

	auto FPostProcessPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FPostProcessPassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::SceneColor), FSceneColorPassResult>(
				"SceneColor", offsetof(FParameters, SceneColor)
			),
			MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::DeferredLighting), FIsolatedDeferredPassResult>(
				"DeferredLighting", offsetof(FParameters, DeferredLighting)
			),
			MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::BasePassDebugCompletion), FGBufferPassResult>("BasePassDebugCompletion", offsetof(FParameters, BasePassDebugCompletion)),
			MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::MaterialCompletion), bool>("MaterialCompletion", offsetof(FParameters, MaterialCompletion)),
			MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::Completion), FPostProcessPassResult>(
				"Completion", offsetof(FParameters, Completion)
			),
			MakeRDGNestedParameterMemberMetadata<FParameters, decltype(FParameters::Resources)>("Resources", offsetof(FParameters, Resources), FPostProcessPassResources::GetRDGParametersMetadata())
		};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FPostProcessPassParameters", Members);
		return &Metadata;
	}

	#undef DURIN_COLOR
	#undef DURIN_TEXTURE

	namespace
	{
		struct FMaterialEffectParameters
		{
			FMaterialSceneTexturePassParameters Scene;
			std::optional<FRDGTextureParameter> Cloud;
			TRDGValueRead<FSceneColorPassResult> Input;
			TRDGValueWrite<bool> Applied;
			static auto GetRDGParametersMetadata() -> const FRDGParametersMetadata*
			{
				using P = FMaterialEffectParameters;
				static const std::array Members{
					MakeRDGNestedParameterMemberMetadata<P, decltype(P::Scene)>("Scene", offsetof(P, Scene), FMaterialSceneTexturePassParameters::GetRDGParametersMetadata()),
					MakeRDGTextureReadMetadata<P, decltype(P::Cloud)>("Cloud", offsetof(P, Cloud)),
					MakeRDGValueParameterMemberMetadata<P, decltype(P::Input), FSceneColorPassResult>("Input", offsetof(P, Input)),
					MakeRDGValueParameterMemberMetadata<P, decltype(P::Applied), bool>("Applied", offsetof(P, Applied))};
				static const auto Metadata = MakeInlineRDGParametersMetadata<P>("FMaterialEffectParameters", Members);
				return &Metadata;
			}
		};
		struct FPostProcessRecorder final
		{
			FGBufferDebugRenderer& GBufferDebugRenderer;
			FPostProcessRenderer& PostProcessRenderer;
			std::shared_ptr<const FPreparedPostProcess> Draw;
			FSceneRenderTelemetry& Telemetry;

			auto RenderPostProcess_RenderThread(FRHICommandListImmediate&,
				const FSceneView&, const FSceneView&, FRHITexture*, bool,
				const FSceneViewRenderOptions&,
				const FPostProcessRenderer::FSceneTargets&,
				const FGBufferRenderer::FTargets*,
				const FGBufferDebugRenderer::FTargets*, FRHITexture*, FRHITexture*, FRHITexture*) -> FPostProcessPassResult;
		};

	} // namespace

	auto AddPostProcessPasses(
		FRDGBuilder& Graph, const FPostProcessFeatureInputs& Inputs) -> FPostProcessGraphOutput
	{
		FPostProcessRecorder Recorder{
			Inputs.GBufferDebug, Inputs.Renderer, Inputs.Draw, Inputs.Telemetry};
		const auto& RecordView = Inputs.RecordView;
		const auto& View = Inputs.View;
		auto* OutputTarget = Inputs.OutputTarget;
		const auto& Options = Inputs.Options;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bPresentOutput = Inputs.bPresentOutput;
		const bool bHasEditorAssistance =
			Inputs.EditorAssistanceFeature.IsEnabled();
		const bool bGBufferDebug = Inputs.GBufferDebugFeature.IsEnabled() && Options.GBufferDebugMode != EGBufferDebugMode::Emissive
			&& Options.GBufferDebugMode != EGBufferDebugMode::MaterialInputs;
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
				"Scene.GBuffer.Debug");
		auto Parameters = Graph.AllocParameters<FPostProcessPassParameters>();
		Parameters->SceneColor = {.Value = Inputs.SceneColor.Completion};
		if (Inputs.Material)
		{
			const auto& Prepared = Inputs.Material;
			const auto Output = Graph.RegisterExternalTexture(Prepared->Output, "Material.PostProcessOutput",
				ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
			FMaterialSceneTextureInputs Textures;
			Textures.PostProcessInput0 = FScreenPassTexture{Inputs.SceneColor.Color,
				{RecordView.ViewportX, RecordView.ViewportY}, {RecordView.ViewportWidth, RecordView.ViewportHeight}};
			Textures.SceneDepth = FScreenPassTexture{Inputs.SceneColor.Depth, Textures.PostProcessInput0->ViewMin, Textures.PostProcessInput0->ViewSize};
			const auto Projection = Math::TransposeToFloat(RecordView.ProjectionMatrix);
			for (uint32 Row = 0; Row < 4; ++Row) Textures.ProjectionRows[Row] = Projection[Row];
			Textures.ClearDepth = RecordView.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.f : 1.f;
			auto Plan = PlanMaterialSceneTextures(Graph, Prepared->Program->GetShaderMap().Entries[0], Textures, Output);
			if (Plan)
			{
				const auto Applied = Graph.CreateValue<bool>("Scene.PostProcessMaterial.Applied", "optional-material-result");
				auto Effect = Graph.AllocParameters<FMaterialEffectParameters>();
				Effect->Scene.PostProcessInput0 = Plan->Parameters->PostProcessInput0;
				Effect->Scene.SceneDepth = Plan->Parameters->SceneDepth;
				Effect->Scene.Output = Plan->Parameters->Output;
				Effect->Input = {Inputs.SceneColor.Completion}; Effect->Applied = {Applied};
				if (Inputs.SceneColor.CloudComposite) Effect->Cloud = {*Inputs.SceneColor.CloudComposite, {ERHITextureAspect::Color, 0, 1, 0, 1}};
				Graph.AddPass("Scene.PostProcessMaterial", ERDGPassType::Graphics, std::move(Effect),
					[Prepared, Uniform = Plan->Uniform, Renderer = &Inputs.Renderer](FRHICommandListImmediate& Commands,
						const FMaterialEffectParameters& P, const FRDGParameterResolver& Resolver) {
						const auto& Result = Resolver.ReadValue(P.Input);
						auto* Input = Resolver.GetTexture(P.Scene.PostProcessInput0);
						if (Result.bUsesVolumetricCloudComposite && P.Cloud) Input = Resolver.GetTexture(P.Cloud);
						Resolver.WriteValue(P.Applied) = Result.IsSuccess() && Renderer->DrawMaterial_RenderThread(
							Commands, *Prepared, Input, Resolver.GetTexture(P.Scene.SceneDepth), Uniform);
					});
				Parameters->MaterialCompletion = TRDGValueRead<bool>{Applied};
				Parameters->Resources.MaterialOutput = {Output, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			}
		}
		if (Inputs.Deferred.Completion)
			Parameters->DeferredLighting = TRDGValueRead<FIsolatedDeferredPassResult>{*Inputs.Deferred.Completion};
		Parameters->Completion = {.Value = PostProcessCompletion};
		if (Inputs.GBuffer.BasePassDebug) Parameters->Resources.BasePassDebug = FRDGTextureParameter{*Inputs.GBuffer.BasePassDebug, {ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (Inputs.GBuffer.BasePassDebugCompletion) Parameters->BasePassDebugCompletion = TRDGValueRead<FGBufferPassResult>{*Inputs.GBuffer.BasePassDebugCompletion};
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
		if (Inputs.GBuffer.Textures && bGBufferDebug)
			SceneTextureGroups::FillGBuffer(Inputs.GBuffer.Textures,
				Parameters->Resources.GBuffer);
		if (Inputs.Deferred.Isolated)
			Parameters->Resources.IsolatedDeferred = {
				*Inputs.Deferred.Isolated,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		const auto PostProcessPass =
			Graph.AddPass(PostProcessPassName, ERDGPassType::Graphics, std::move(Parameters),
			[Recorder,
				RecordView = &RecordView, &View, bGBufferDebug, &Options,
				bPresentOutput](
				FRHICommandListImmediate& Commands,
				const FPostProcessPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) mutable {
				const auto& SceneColorResult = Resolver.ReadValue(
					PassParameters.SceneColor);
				auto& PostProcessResult = Resolver.WriteValue(
					PassParameters.Completion);
				if (!SceneColorResult.IsSuccess())
				{
					PostProcessResult.Result = SceneColorResult.Result;
					return;
				}
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
				FRHITexture* MaterialColorInput = nullptr;
				if (const auto* Applied = Resolver.ReadValue(PassParameters.MaterialCompletion); Applied && *Applied)
					MaterialColorInput = Resolver.GetTexture(PassParameters.Resources.MaterialOutput);
				std::optional<FGBufferDebugRenderer::FTargets> DebugTargets;
				if (PassParameters.Resources.GBufferDebugOutput)
					DebugTargets = {.Color = Resolver.GetColorAttachment(
						PassParameters.Resources.GBufferDebugOutput).Texture};
				const auto GBufferTargets = SceneTextureGroups::ResolveGBuffer(
					Resolver, PassParameters.Resources.GBuffer);
				FRHITexture* IsolatedDeferredOutput = nullptr;
				if (PassParameters.Resources.IsolatedDeferred
					&& Resolver.ReadValue(PassParameters.DeferredLighting)
					&& Resolver.ReadValue(PassParameters.DeferredLighting)->bOutputValid)
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
				auto LocalOptions = Options;
				if (Options.GBufferDebugMode == EGBufferDebugMode::Emissive
					|| Options.GBufferDebugMode == EGBufferDebugMode::MaterialInputs)
				{
					LocalOptions.GBufferDebugMode = EGBufferDebugMode::Disabled;
					const auto* Debug = Resolver.ReadValue(PassParameters.BasePassDebugCompletion);
					if (Debug && Debug->IsComplete())
					{
						SceneColorInput = Resolver.GetTexture(PassParameters.Resources.BasePassDebug);
						++Recorder.Telemetry.View.GBuffer.GBufferDebugViews;
					}
					else
						++Recorder.Telemetry.View.GBuffer.GBufferDebugFailures;
				}
				PostProcessResult = Recorder.RenderPostProcess_RenderThread(
					Commands, *RecordView, View,
					Output,
					bPresentOutput, LocalOptions, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					DebugTargets ? &*DebugTargets : nullptr,
					SceneColorInput, MaterialColorInput, IsolatedDeferredOutput
				);
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
		FRHITexture* MaterialColor,
		FRHITexture* GroundTruthAmbientOcclusionDebugOutput
	) -> FPostProcessPassResult
	{
		const uint32 Width = OutputTarget->GetSizeX();
		const uint32 Height = OutputTarget->GetSizeY();
		FRHITexture* PostProcessInput = MaterialColor ? MaterialColor : SceneColor;
		if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled
			&& GBufferTargets != nullptr)
		{
			if (GBufferDebugTargets != nullptr
				&& GBufferDebugRenderer.Render_RenderThread(
					CommandList,
					GBufferTargets->Material,
					GBufferTargets->Normals,
					GBufferTargets->Surface,
					GBufferTargets->Material,
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

		const FPostProcessTimingQuerySink PostProcessTimingSink =
			GetPostProcessTimingQuerySink();
		TScopedRendererGPUTimingQuery PostProcessTiming(
			CommandList, PostProcessTimingSink
		);
		PostProcessRenderer.Draw_RenderThread(CommandList, PostProcessInput, OutputTarget, *Draw,
			FClearValueBinding(View.ClearColor.r, View.ClearColor.g, View.ClearColor.b, View.ClearColor.a),
			bPresentOutput, RenderView.Settings.PostProcess.ExposureEV);
		PostProcessTiming.Commit();
		return {.Result = ERenderViewResult::Success};
	}
} // namespace Durin
