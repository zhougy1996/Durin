#include "Renderers/SceneColorRendering.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	auto FSceneColorPassResources::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FSceneColorPassResources;
		#define DURIN_MANAGED(Field, Entry, Discard, Result) \
			MakeRDGResourceParameterMemberMetadata<FParameters, \
				decltype(FParameters::Field), FRDGManagedTextureParameter>(#Field, \
					offsetof(FParameters, Field), ERDGParameterMemberKind::ManagedTexture, \
					ERDGResourceKind::Texture, \
					ERDGParameterRangeKind::TextureSubresource, ERDGUse::ReadWrite, Entry, \
					Discard, ERHIRenderTargetLoadAction::Load, \
					ERHIRenderTargetStoreAction::Store, true, Result)
		static const std::array Members = {
			DURIN_MANAGED(SceneColorManaged, ERHIAccess::ColorAttachmentReadWrite,
				false, ERHIAccess::GraphicsShaderRead),
			DURIN_MANAGED(SceneDepthManaged, ERHIAccess::GraphicsShaderRead,
				false, ERHIAccess::DepthStencilReadWrite)};
		#undef DURIN_MANAGED
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FSceneColorPassResources", Members);
		return &Metadata;
	}

	auto FSceneColorPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FSceneColorPassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::BaseScene), FSceneColorPassResult>(
					"BaseScene", offsetof(FParameters, BaseScene)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::VolumetricCloud), FVolumetricCloudPassResult>(
					"VolumetricCloud", offsetof(FParameters, VolumetricCloud)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::Completion), FSceneColorPassResult>(
					"Completion", offsetof(FParameters, Completion)),
			MakeRDGNestedParameterMemberMetadata<FParameters,
				decltype(FParameters::Resources)>("Resources",
					offsetof(FParameters, Resources),
					FSceneColorPassResources::GetRDGParametersMetadata())};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FSceneColorPassParameters", Members);
		return &Metadata;
	}

	namespace
	{
		struct FSceneColorRecorder final
		{
			FStaticMeshRenderer& StaticMeshRenderer;
			FSkeletalMeshRenderer& SkeletalMeshRenderer;
			FSceneRenderTelemetry& Telemetry;
			FResolvedSceneResources& ResolvedSceneResources;

			auto RenderSceneTranslucency_RenderThread(
				FRHICommandListImmediate&, const FSceneGeometryRecordInputs&,
				FRHITexture*, FRHITexture*, const FSceneColorPassResult&,
				const FVolumetricCloudPassResult&) -> FSceneColorPassResult;
		};
	} // namespace

	auto FSceneColorRendering::AddPasses(
		const FSceneColorFeatureInputs& Inputs) -> FSceneColorGraphOutput
	{
		auto& Graph = Inputs.Graph;
		FSceneColorRecorder Recorder{Inputs.StaticMeshes,
			Inputs.SkeletalMeshes, Inputs.Telemetry, Inputs.Resolved};
		const auto RecordInputs = Inputs.Record;
		const bool bRequiresDeferredOpaque =
			Inputs.DeferredFeature.HasPurpose(ESceneFeaturePurpose::Production);
		const bool bVolumetricCloudComposite = Inputs.CloudFeature.Decision.Route
			!= FVolumetricCloudRenderer::ERoute::Disabled;
		const auto SceneColorCompletion = Graph.CreateValue<FSceneColorPassResult>(
			"Scene.ColorValue", "scene-color-result");
		auto Parameters = Graph.AllocParameters<FSceneColorPassParameters>();
		Parameters->BaseScene = {.Value = Inputs.BaseScene.Completion};
		Parameters->VolumetricCloud = {.Value = Inputs.VolumetricCloud.Completion};
		Parameters->Completion = {.Value = SceneColorCompletion};
		if (bRequiresDeferredOpaque)
		{
			const FRDGTextureHandle Color =
				bVolumetricCloudComposite
					&& Inputs.VolumetricCloud.Composite
				? *Inputs.VolumetricCloud.Composite
				: Inputs.BaseScene.Color;
			Parameters->Resources.SceneColorManaged = {
				.Texture = Color,
				.Range = {ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.SceneDepthManaged = {
				.Texture = Inputs.BaseScene.Depth,
				.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		}
		(void)Graph.AddPass(Name, ERDGPassType::Graphics, std::move(Parameters),
			[Recorder, &Publication = Inputs.Publication,
				RecordInputs, bVolumetricCloudComposite,
				bRequiresDeferredOpaque](
				FRHICommandListImmediate& Commands,
				const FSceneColorPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) mutable {
				auto& SceneColorResult = Resolver.WriteValue(
					PassParameters.Completion);
				const auto& BaseSceneResult = Resolver.ReadValue(
					PassParameters.BaseScene);
				const auto& VolumetricCloudResult = Resolver.ReadValue(
					PassParameters.VolumetricCloud);
				if (!bRequiresDeferredOpaque)
					SceneColorResult = BaseSceneResult;
				else
				{
					FSceneColorPassResult Input = BaseSceneResult;
					if (bVolumetricCloudComposite
						&& !VolumetricCloudResult.bCompositeOutputValid)
						Input.Result = ERenderViewResult::RendererResourcesUnavailable;
					FRHITexture* Color = Resolver.GetTexture(
						PassParameters.Resources.SceneColorManaged);
					SceneColorResult = Recorder.RenderSceneTranslucency_RenderThread(
						Commands,
						RecordInputs,
						Color,
						Resolver.GetTexture(
							PassParameters.Resources.SceneDepthManaged), Input,
						VolumetricCloudResult);
				}
				Publication = SceneColorResult;
				if (!SceneColorResult.IsSuccess()) return;
				ReduceStaticMeshTelemetry(RecordInputs.Receiver.StaticMeshes,
					Recorder.ResolvedSceneResources.Receiver.StaticMeshes, Recorder.Telemetry.View);
				ReduceSkeletalMeshTelemetry(RecordInputs.Receiver.SkeletalMeshes,
					Recorder.ResolvedSceneResources.Receiver.SkeletalMeshes,
					Recorder.ResolvedSceneResources.Receiver.SkeletalPalettes, Recorder.Telemetry.View);
			});
		return {.Completion = SceneColorCompletion,
			.Color = Inputs.BaseScene.Color, .Depth = Inputs.BaseScene.Depth,
			.CloudComposite = Inputs.VolumetricCloud.Composite};
	}

	auto FSceneColorRecorder::RenderSceneTranslucency_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneGeometryRecordInputs& Inputs,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
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
		if (SceneColor == nullptr || Depth == nullptr) return {};
		auto SetViewRect = [&CommandList, &View]() {
			CommandList.SetViewport(
				static_cast<float>(View.ViewportX),
				static_cast<float>(View.ViewportY), 0.0f,
				static_cast<float>(View.ViewportX + View.ViewportWidth),
				static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
			CommandList.SetScissor(
				static_cast<float>(View.ViewportX),
				static_cast<float>(View.ViewportY),
				static_cast<float>(View.ViewportWidth),
				static_cast<float>(View.ViewportHeight));
		};
		const FSortedTranslucencyTimingQuerySink SortedTranslucencyTimingSink =
			GetSortedTranslucencyTimingQuerySink();
		TScopedRendererGPUTimingQuery SortedTranslucencyTiming(
			CommandList, SortedTranslucencyTimingSink
		);

		FRHIRenderPassInfo SortedTranslucency{};
		SortedTranslucency.RenderTargetLayout =
			RenderTargetLayouts::MakeHybridSortedTranslucency();
		SortedTranslucency.ColorRenderTargets[0] = SceneColor;
		SortedTranslucency.DepthStencilRenderTarget = Depth;
		CommandList.BeginRenderPass(
			SortedTranslucency, "HybridSortedTranslucencyRenderPass"
		);
		SetViewRect();
		for (const FPreparedTranslucentSceneDraw& Draw :
			 Inputs.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.StaticMeshes,
					ResolvedSceneResources.Receiver.StaticMeshes, true
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.SkeletalMeshes,
					ResolvedSceneResources.Receiver.SkeletalMeshes, true
				);
		}
		CommandList.EndRenderPass();
		SortedTranslucencyTiming.Commit();
		// Lit opaque/masked sections were already consumed by GBuffer + deferred
		// lighting, so the retained-forward attempted count intentionally does not
		// equal every prepared section as it does in the all-forward finalizer.
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedSceneResources.Receiver.StaticMeshes);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedSceneResources.Receiver.SkeletalMeshes);
		++Telemetry.View.Deferred.HybridDeferredEnabledViews;
		return {
			.Result = ERenderViewResult::Success,
			.bUsesVolumetricCloudComposite =
				VolumetricCloud.bCompositeOutputValid,
			.VolumetricCloud = VolumetricCloud};
	}
} // namespace Durin
