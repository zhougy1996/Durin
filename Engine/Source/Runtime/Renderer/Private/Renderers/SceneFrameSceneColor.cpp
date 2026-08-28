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
	auto FSceneColorGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneGeometryRecordInputs& RecordInputs) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Topology = Context.Topology;
		const auto& PreparedEditorAssistance =
			Context.EditorAssistance;
		const auto PreparedContactRoute = Context.ContactRoute;
		const auto PreparedCloudShadowRoute = Context.CloudShadowRoute;
		const auto PreparedCloudRoute = Context.CloudRoute;
		auto* CloudWeatherTexture = Context.CloudWeatherTexture;
		auto* DirectionalShadowTexture = Context.DirectionalShadowTexture;
		const uint32 Width = Context.Width;
		const uint32 Height = Context.Height;
		const bool bPresentOutput = Context.bPresentOutput;
		const bool bHasEditorAssistance =
			Context.bHasEditorAssistance;
		const bool bRequiresDeferredOpaque =
			Context.bRequiresDeferredOpaque;
		const bool bWantsIsolatedDeferred =
			Context.bWantsIsolatedDeferred;
		const bool bWantsGroundTruthAmbientOcclusion =
			Context.bWantsGroundTruthAmbientOcclusion;
		const bool bWantsDeferredInputs =
			Context.bWantsDeferredInputs;
		const bool bWantsProductionDeferred =
			Context.bWantsProductionDeferred;
		const bool bHybridRetainedResourcesReady =
			Context.bHybridRetainedResourcesReady;
		const bool bNeedsGBuffer = Context.bNeedsGBuffer;
		auto& DeferredParameters =
			Context.Composition.DeferredParameters;
		auto& ProductionDeferredParameters =
			Context.Composition.ProductionDeferredParameters;
		auto& GraphResources = Context.Composition.Resources;
		auto& Channels = Context.Composition.Channels;
		auto& DirectionalShadowValue = Channels.DirectionalShadow;
		auto& GBufferValue = Channels.GBuffer;
		auto& AmbientOcclusionValue = Channels.AmbientOcclusion;
		auto& ContactShadowVisibilityValue = Channels.ContactShadowVisibility;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredDirectionalLightingValue = Channels.DeferredDirectionalLighting;
		auto& BaseSceneValue = Channels.BaseScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto DeclarePersistentGraphicsInputs = [&](auto Pass) {
			std::vector<FRenderGraphTextureHandle> Declared;
			auto Declare = [&](const auto& Handle, FRHITexture* Physical) {
				if (!Handle || !Physical
					|| std::ranges::find(Declared, *Handle) != Declared.end())
					return;
				Declared.push_back(*Handle);
				Graph.UseTexture(Pass, *Handle,
					{GetTextureAspects(Physical->GetFormat()), 0,
						Physical->GetNumMips(), 0, Physical->GetArraySize()},
					ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
			};
			Declare(GraphResources.DefaultWhite,
				Services.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
			Declare(GraphResources.DefaultShadowArray,
				Services.DefaultTextures.GetArray_RenderThread());
			Declare(GraphResources.EnvironmentIrradiance,
				GraphResources.SelectedEnvironmentIrradiance);
			Declare(GraphResources.EnvironmentPrefiltered,
				GraphResources.SelectedEnvironmentPrefiltered);
			Declare(GraphResources.EnvironmentBrdfLut,
				GraphResources.SelectedEnvironmentBrdfLut);
		};
		const auto SceneColorPass =
			AddSceneFrameFeaturePass<FSceneColorGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, &Composition = Context.Composition,
				RecordInputs, &GraphResources, &Topology,
				bRequiresDeferredOpaque](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				auto& SceneColorResult = Resources.WriteValue(
					Channels.SceneColor.Handle);
				const auto& BaseSceneResult = Resources.ReadValue(
					Channels.BaseScene.Handle);
				const auto& VolumetricCloudResult = Resources.ReadValue(
					Channels.VolumetricCloud.Handle);
				if (!bRequiresDeferredOpaque)
					SceneColorResult = BaseSceneResult;
				else
				{
					FSceneColorPassResult Input = BaseSceneResult;
					if (Topology.bVolumetricCloudComposite
						&& !VolumetricCloudResult.bCompositeOutputValid)
						Input.Result = ERenderViewResult::RendererResourcesUnavailable;
					FRHITexture* Color = Topology.bVolumetricCloudComposite
						&& GraphResources.VolumetricCloudComposite
						? Resources.GetTexture(
							*GraphResources.VolumetricCloudComposite)
						: Resources.GetTexture(GraphResources.SceneColor);
					SceneColorResult = Services.Recorders.RenderSceneTranslucency_RenderThread(
						Commands,
						RecordInputs,
						Color,
						Resources.GetTexture(GraphResources.SceneDepth), Input,
						VolumetricCloudResult);
				}
				Composition.SceneColorPublication = SceneColorResult;
				if (!SceneColorResult.IsSuccess()) return;
				ReduceStaticMeshTelemetry(RecordInputs.Receiver.StaticMeshes,
					Services.ResolvedFrame.Receiver.StaticMeshes, Services.Telemetry.View);
				ReduceSkeletalMeshTelemetry(RecordInputs.Receiver.SkeletalMeshes,
					Services.ResolvedFrame.Receiver.SkeletalMeshes,
					Services.ResolvedFrame.Receiver.SkeletalPalettes, Services.Telemetry.View);
				ReduceTerrainTelemetry(RecordInputs.Receiver.Terrains,
					Services.ResolvedFrame.Receiver.Terrains, Services.Telemetry.View);
			});
		Graph.UseValue(SceneColorPass, BaseSceneValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseValue(SceneColorPass, VolumetricCloudValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseValue(SceneColorPass, SceneColorValue.Handle,
			ERenderGraphUse::Write);
		if (bRequiresDeferredOpaque)
		{
			const FRenderGraphTextureHandle Color =
				Topology.bVolumetricCloudComposite
					&& GraphResources.VolumetricCloudComposite
				? *GraphResources.VolumetricCloudComposite
				: GraphResources.SceneColor;
			Graph.UseManagedTexture(SceneColorPass, Color,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite,
				ERHIAccess::ColorAttachmentReadWrite,
				ERHIAccess::GraphicsShaderRead);
			Graph.UseManagedTexture(SceneColorPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::DepthStencilReadWrite);
		}
	}

	auto FSceneFrameFeatureRecorders::RenderSceneTranslucency_RenderThread(
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
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.StaticMeshes,
					ResolvedFrame.Receiver.StaticMeshes, true
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes, true
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					Inputs.Receiver.Terrains.Translucent[Draw.DrawIndex],
					Inputs.Receiver.Terrains,
					ResolvedFrame.Receiver.Terrains, true
				);
		}
		CommandList.EndRenderPass();
		SortedTranslucencyTiming.Commit();
		// Lit opaque/masked sections were already consumed by GBuffer + deferred
		// lighting, so the retained-forward attempted count intentionally does not
		// equal every prepared section as it does in the all-forward finalizer.
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.StaticMeshes);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.SkeletalMeshes);
		TerrainRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.Terrains);
		++Telemetry.View.Deferred.HybridDeferredEnabledViews;
		return {
			.Result = ERenderViewResult::Success,
			.bUsesVolumetricCloudComposite =
				VolumetricCloud.bCompositeOutputValid,
			.VolumetricCloud = VolumetricCloud};
	}
} // namespace Durin
