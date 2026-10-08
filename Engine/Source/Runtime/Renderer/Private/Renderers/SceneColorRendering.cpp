#include "Renderers/SceneColorRendering.h"
#include "RDG/RDG.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"

namespace Durin
{
	auto FSceneColorPassResources::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FSceneColorPassResources;
		static const std::array Members = {
			MakeRDGColorAttachmentBindingMetadata<FParameters, decltype(FParameters::SceneColorOutput)>("SceneColorOutput", offsetof(FParameters, SceneColorOutput)),
			MakeRDGDepthStencilAttachmentBindingMetadata<FParameters, decltype(FParameters::SceneDepthOutput)>("SceneDepthOutput", offsetof(FParameters, SceneDepthOutput))
		};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FSceneColorPassResources", Members);
		return &Metadata;
	}

	auto FSceneColorPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FSceneColorPassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::BaseScene), FSceneColorPassResult>(
				"BaseScene", offsetof(FParameters, BaseScene)
			),
			MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::VolumetricCloud), FVolumetricCloudPassResult>(
				"VolumetricCloud", offsetof(FParameters, VolumetricCloud)
			),
			MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::Completion), FSceneColorPassResult>(
				"Completion", offsetof(FParameters, Completion)
			),
			MakeRDGNestedParameterMemberMetadata<FParameters, decltype(FParameters::Resources)>("Resources", offsetof(FParameters, Resources), FSceneColorPassResources::GetRDGParametersMetadata())
		};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FSceneColorPassParameters", Members);
		return &Metadata;
	}

	namespace
	{
		struct FSceneColorRecorder final
		{
			FStaticMeshRenderer& StaticMeshRenderer;
			FSceneRenderTelemetry& Telemetry;
			FResolvedSceneResources& ResolvedSceneResources;

			auto RenderSceneTranslucency_RenderThread(
				FRHICommandListImmediate&, const FSceneGeometryRecordInputs&, const FRHIRenderPassInfo&, const FSceneColorPassResult&, const FVolumetricCloudPassResult&
			) -> FSceneColorPassResult;
		};
	} // namespace

	auto AddSceneColorPasses(
		FRDGBuilder& Graph, const FSceneColorFeatureInputs& Inputs
	) -> FSceneColorGraphOutput
	{
		FSceneColorRecorder Recorder{Inputs.StaticMeshes, Inputs.Telemetry, Inputs.Resolved};
		const auto RecordInputs = Inputs.Record;
		const auto Failure = GetRendererQualificationPolicy().RasterFailure;
		const bool bRequiresDeferredOpaque =
			Inputs.DeferredFeature.HasPurpose(ESceneFeaturePurpose::Production);
		const bool bVolumetricCloudComposite = Inputs.CloudFeature.Decision.Route
											   != FVolumetricCloudRenderer::ERoute::Disabled;
		const auto SceneColorCompletion = Graph.CreateValue<FSceneColorPassResult>(
			"Scene.ColorValue", "scene-color-result"
		);
		auto Parameters = Graph.AllocParameters<FSceneColorPassParameters>();
		Parameters->BaseScene = {.Value = Inputs.BaseScene.Completion};
		if (Inputs.VolumetricCloud.Completion)
			Parameters->VolumetricCloud = TRDGValueRead<FVolumetricCloudPassResult>{
				.Value = *Inputs.VolumetricCloud.Completion
			};
		Parameters->Completion = {.Value = SceneColorCompletion};
		if (bRequiresDeferredOpaque)
		{
			const FRDGTextureHandle Color =
				bVolumetricCloudComposite
						&& Inputs.VolumetricCloud.Composite ?
					*Inputs.VolumetricCloud.Composite :
					Inputs.BaseScene.Color;
			Parameters->Resources.SceneColorOutput = {
				.Texture = Color,
				.Range = {ERHITextureAspect::Color, 0, 1, 0, 1}
			};
			Parameters->Resources.SceneDepthOutput = {
				.Texture = Inputs.BaseScene.Depth,
				.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}
			};
		}
		(void)Graph.AddPass(SceneColorPassName, ERDGPassType::Graphics, std::move(Parameters), [Recorder, &Publication = Inputs.Publication, RecordInputs, bVolumetricCloudComposite, Failure, bRequiresDeferredOpaque](FRHICommandListImmediate& Commands, const FSceneColorPassParameters& PassParameters, const FRDGParameterResolver& Resolver) mutable {
			auto& SceneColorResult = Resolver.WriteValue(
				PassParameters.Completion
			);
			const auto& BaseSceneResult = Resolver.ReadValue(
				PassParameters.BaseScene
			);
			const auto* VolumetricCloudValue = Resolver.ReadValue(
				PassParameters.VolumetricCloud
			);
			const auto VolumetricCloudResult = VolumetricCloudValue ? *VolumetricCloudValue : FVolumetricCloudPassResult{};
			if (!bRequiresDeferredOpaque)
				SceneColorResult = BaseSceneResult;
			else
			{
				FSceneColorPassResult Input = BaseSceneResult;
				if (Input.IsSuccess() && bVolumetricCloudComposite
					&& !VolumetricCloudResult.bCompositeOutputValid)
					Input.Result = ERenderViewResult::RendererResourcesUnavailable;
				if (Input.IsSuccess() && Failure == ESceneRasterFailure::SortedTranslucency) Input = {};
				SceneColorResult = Input;
				if (Input.IsSuccess())
				{
					FRHIRenderPassInfo Native;
					const auto Color = MakeRDGNativeAttachmentBinding(Resolver.GetColorAttachment(PassParameters.Resources.SceneColorOutput));
					const auto Depth = MakeRDGNativeAttachmentBinding(Resolver.GetDepthStencilAttachment(PassParameters.Resources.SceneDepthOutput));
					Native.RenderTargetLayout.NumColorRenderTargets = 1;
					Native.RenderTargetLayout.ColorAttachments[0].RenderTarget = Color.Layout;
					Native.RenderTargetLayout.bHasDepthStencil = true;
					Native.RenderTargetLayout.DepthStencilAttachment = Depth.Layout;
					Color.BindColor(Native, 0);
					Depth.BindDepthStencil(Native);
					SceneColorResult = Recorder.RenderSceneTranslucency_RenderThread(Commands, RecordInputs, Native, Input, VolumetricCloudResult);
				}
			}
			Publication = SceneColorResult;
			if (!SceneColorResult.IsSuccess()) return;
			ReduceStaticMeshTelemetry(RecordInputs.Receiver.StaticMeshes, Recorder.ResolvedSceneResources.Receiver.StaticMeshes, Recorder.Telemetry.View);
		});
		return {.Completion = SceneColorCompletion, .Color = Inputs.BaseScene.Color, .Depth = Inputs.BaseScene.Depth, .CloudComposite = Inputs.VolumetricCloud.Composite};
	}

	auto FSceneColorRecorder::RenderSceneTranslucency_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneGeometryRecordInputs& Inputs,
		const FRHIRenderPassInfo& Native,
		const FSceneColorPassResult& BaseScene,
		const FVolumetricCloudPassResult& VolumetricCloud
	) -> FSceneColorPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (!BaseScene.IsSuccess()) return BaseScene;
		const FSceneView& View = Inputs.View;
		if (View.Settings.Mode.RenderMode != ERenderMode::Lit
			|| View.Settings.Mode.RasterMode != ERasterMode::Solid)
			return BaseScene;
		if (Native.ColorRenderTargets[0] == nullptr || Native.DepthStencilRenderTarget == nullptr) return {};
		auto SetViewRect = [&CommandList, &View]() {
			CommandList.SetViewport(
				static_cast<float>(View.ViewportX),
				static_cast<float>(View.ViewportY), 0.0f,
				static_cast<float>(View.ViewportX + View.ViewportWidth),
				static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f
			);
			CommandList.SetScissor(
				static_cast<float>(View.ViewportX),
				static_cast<float>(View.ViewportY),
				static_cast<float>(View.ViewportWidth),
				static_cast<float>(View.ViewportHeight)
			);
		};
		const FSortedTranslucencyTimingQuerySink SortedTranslucencyTimingSink =
			GetSortedTranslucencyTimingQuerySink();
		TScopedRendererGPUTimingQuery SortedTranslucencyTiming(
			CommandList, SortedTranslucencyTimingSink
		);

		CommandList.BeginRenderPass(Native, "HybridSortedTranslucencyRenderPass");
		SetViewRect();
		FMeshDrawBindingGroup TranslucentBindings;
		for (const FPreparedTranslucentSceneDraw& Draw :
			 Inputs.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.StaticMeshes,
					ResolvedSceneResources.Receiver.StaticMeshes, true, &TranslucentBindings
				);
		}
		CommandList.EndRenderPass();
		SortedTranslucencyTiming.Commit();
		// Lit opaque/masked sections were already consumed by GBuffer + deferred
		// lighting, so the retained-forward attempted count intentionally does not
		// equal every prepared section as it does in the all-forward finalizer.
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedSceneResources.Receiver.StaticMeshes
		);
		++Telemetry.View.Deferred.HybridDeferredEnabledViews;
		return {
			.Result = ERenderViewResult::Success,
			.bUsesVolumetricCloudComposite =
				VolumetricCloud.bCompositeOutputValid,
			.VolumetricCloud = VolumetricCloud
		};
	}
} // namespace Durin
