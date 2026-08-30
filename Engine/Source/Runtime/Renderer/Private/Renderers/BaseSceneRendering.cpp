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
	auto FBaseSceneGraphContributor::AddPasses(
		const FBaseSceneGraphInputs& Inputs) -> FBaseSceneGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto RecordInputs = Inputs.Record;
		const bool bRequiresDeferredOpaque = Inputs.bRequiresDeferredOpaque;
		const bool bNeedsGBuffer = Inputs.bNeedsGBuffer;
		auto& ProductionDeferredParameters = Inputs.ProductionDeferredParameters;
		struct {
			std::optional<FRDGTextureHandle> DirectionalShadow;
			FRDGTextureHandle SceneColor;
			FRDGTextureHandle SceneDepth;
			std::optional<FRDGTextureHandle> DefaultWhite;
			std::optional<FRDGTextureHandle> DefaultShadowArray;
			std::optional<FRDGTextureHandle> EnvironmentIrradiance;
			std::optional<FRDGTextureHandle> EnvironmentPrefiltered;
			std::optional<FRDGTextureHandle> EnvironmentBrdfLut;
			FRHITexture* SelectedEnvironmentIrradiance = nullptr;
			FRHITexture* SelectedEnvironmentPrefiltered = nullptr;
			FRHITexture* SelectedEnvironmentBrdfLut = nullptr;
		} GraphResources;
		GraphResources.SceneColor = Inputs.SceneColor;
		GraphResources.SceneDepth = Inputs.SceneDepth;
		GraphResources.DirectionalShadow = Inputs.DirectionalShadow.Shadow;
		GraphResources.DefaultWhite = Inputs.DefaultWhite;
		GraphResources.DefaultShadowArray = Inputs.DefaultShadowArray;
		GraphResources.EnvironmentIrradiance = Inputs.EnvironmentIrradiance;
		GraphResources.EnvironmentPrefiltered = Inputs.EnvironmentPrefiltered;
		GraphResources.EnvironmentBrdfLut = Inputs.EnvironmentBrdfLut;
		GraphResources.SelectedEnvironmentIrradiance = Inputs.SelectedEnvironmentIrradiance;
		GraphResources.SelectedEnvironmentPrefiltered = Inputs.SelectedEnvironmentPrefiltered;
		GraphResources.SelectedEnvironmentBrdfLut = Inputs.SelectedEnvironmentBrdfLut;
		struct {
			TRDGValueHandle<FIsolatedDeferredPassResult>
				DeferredDirectionalLighting;
			TRDGValueHandle<FSceneColorPassResult> BaseScene;
		} Channels;
		Channels.DeferredDirectionalLighting = Inputs.Deferred.Completion;
		Channels.BaseScene = Graph.CreateValue<FSceneColorPassResult>(
			"Scene.BaseValue", "scene-color-result");
		auto Parameters = Graph.AllocParameters<FBaseScenePassParameters>();
		Parameters->DeferredLighting = {
			.Value = Channels.DeferredDirectionalLighting};
		Parameters->Completion = {.Value = Channels.BaseScene};
		std::vector<FRDGTextureHandle> DeclaredPersistentInputs;
		auto AssignRead = [&DeclaredPersistentInputs](auto& Parameter, const auto& Handle,
			FRHITexture* Physical) {
			if (!Handle || !Physical
				|| std::ranges::find(DeclaredPersistentInputs, *Handle)
					!= DeclaredPersistentInputs.end()) return;
			DeclaredPersistentInputs.push_back(*Handle);
			Parameter = FRDGTextureParameter{*Handle,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()}};
		};
		AssignRead(Parameters->Resources.DirectionalShadow,
			GraphResources.DirectionalShadow,
			Services.DirectionalShadowRenderer.GetTexture_RenderThread());
		AssignRead(Parameters->Resources.DefaultWhite, GraphResources.DefaultWhite,
			Services.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
		AssignRead(Parameters->Resources.DefaultShadowArray,
			GraphResources.DefaultShadowArray,
			Services.DefaultTextures.GetArray_RenderThread());
		AssignRead(Parameters->Resources.EnvironmentIrradiance,
			GraphResources.EnvironmentIrradiance,
			GraphResources.SelectedEnvironmentIrradiance);
		AssignRead(Parameters->Resources.EnvironmentPrefiltered,
			GraphResources.EnvironmentPrefiltered,
			GraphResources.SelectedEnvironmentPrefiltered);
		AssignRead(Parameters->Resources.EnvironmentBrdfLut,
			GraphResources.EnvironmentBrdfLut,
			GraphResources.SelectedEnvironmentBrdfLut);
		Parameters->Resources.SceneColorOutput = {
			.Texture = GraphResources.SceneColor,
			.Range = {ERHITextureAspect::Color, 0, 1, 0, 1}};
		FRDGManagedTextureParameter Depth{
			.Texture = GraphResources.SceneDepth,
			.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		if (bNeedsGBuffer && bRequiresDeferredOpaque)
			Parameters->Resources.SceneDepthGraphicsToGraphics = Depth;
		else if (bNeedsGBuffer)
			Parameters->Resources.SceneDepthGraphicsToDepth = Depth;
		else if (bRequiresDeferredOpaque)
			Parameters->Resources.SceneDepthDepthToGraphics = Depth;
		else
			Parameters->Resources.SceneDepthDepthToDepth = Depth;
		(void)AddSceneRenderFeaturePass<FBaseSceneGraphContributor>(
			Graph, ERDGPassType::Graphics, std::move(Parameters),
			[&Services, RecordInputs, &ProductionDeferredParameters](
				FRHICommandListImmediate& Commands,
				const FBaseScenePassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = Resolver.GetColorAttachment(
						PassParameters.Resources.SceneColorOutput).Texture,
					.Depth = Resolver.GetTexture(
						PassParameters.Resources.SceneDepthGraphicsToGraphics)};
				if (SceneTargets.Depth == nullptr)
					SceneTargets.Depth = Resolver.GetTexture(
						PassParameters.Resources.SceneDepthGraphicsToDepth);
				if (SceneTargets.Depth == nullptr)
					SceneTargets.Depth = Resolver.GetTexture(
						PassParameters.Resources.SceneDepthDepthToGraphics);
				if (SceneTargets.Depth == nullptr)
					SceneTargets.Depth = Resolver.GetTexture(
						PassParameters.Resources.SceneDepthDepthToDepth);
				const FSceneColorTimingQuerySink TimingSink =
					GetSceneColorTimingQuerySink();
				TScopedRendererGPUTimingQuery Timing(Commands, TimingSink);
				Resolver.WriteValue(PassParameters.Completion) = Services.Recorders.RenderBaseScene_RenderThread(
					Commands,
					RecordInputs,
					SceneTargets.Color, SceneTargets.Depth,
					ProductionDeferredParameters
						? &*ProductionDeferredParameters : nullptr);
				Timing.Commit();
			});
		return {.Completion = Channels.BaseScene,
			.Color = GraphResources.SceneColor, .Depth = GraphResources.SceneDepth};
	}

	auto FSceneRenderFeatureRecorders::RenderBaseScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneGeometryRecordInputs& Inputs,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		const FDeferredDirectionalLightingRenderer::FRenderParameters*
			DeferredParameters
	) -> FSceneColorPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = Inputs.View;
		if (SceneColor == nullptr || Depth == nullptr)
			return {};
		if (View.Settings.Mode.RenderMode != ERenderMode::Lit
			|| View.Settings.Mode.RasterMode != ERasterMode::Solid)
		{
			FRHIRenderPassInfo ScenePassInfo{};
			ScenePassInfo.RenderTargetLayout =
				RenderTargetLayouts::MakeSceneTargets();
			ScenePassInfo.ColorRenderTargets[0] = SceneColor;
			ScenePassInfo.DepthStencilRenderTarget = Depth;
			ScenePassInfo.ColorClearValues[0] = FClearValueBinding(
				View.ClearColor.r, View.ClearColor.g,
				View.ClearColor.b, View.ClearColor.a
			);
			ScenePassInfo.DepthStencilClearValue = FClearValueBinding(
				View.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f,
				0u
			);
			CommandList.BeginRenderPass(ScenePassInfo, "SceneColorRenderPass");
			const bool bRendered = RenderForwardScene_RenderThread(
				CommandList, Inputs, SceneColor
			);
			CommandList.EndRenderPass();
			return {
				.Result = bRendered ? ERenderViewResult::Success
					: ERenderViewResult::RequiredEnvironmentUnavailable};
		}
		if (DeferredParameters == nullptr)
		{
			++Telemetry.View.Deferred.HybridDeferredUnavailableViews;
			return {};
		}

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

		FRHIRenderPassInfo Bootstrap{};
		Bootstrap.RenderTargetLayout = RenderTargetLayouts::MakeHybridSceneBootstrap();
		Bootstrap.ColorRenderTargets[0] = SceneColor;
		Bootstrap.DepthStencilRenderTarget = Depth;
		Bootstrap.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r, View.ClearColor.g,
			View.ClearColor.b, View.ClearColor.a
		);
		CommandList.BeginRenderPass(Bootstrap, "HybridSceneBootstrapRenderPass");
		SetViewRect();
		bool bBootstrapRendered = true;
		if (Inputs.Environment != nullptr)
		{
			if (Inputs.Environment->Texture != nullptr)
			{
				bBootstrapRendered = SkyBoxRenderer.DrawTexture_RenderThread(
					CommandList, View, Inputs.Environment->Texture,
					Inputs.Environment->SkyBox, true
				);
			}
			else
			{
				SkyBoxRenderer.Draw_RenderThread(
					CommandList, View, Inputs.Environment->SkyBox, true
				);
			}
		}
		CommandList.EndRenderPass();
		if (!bBootstrapRendered)
		{
			return {
				.Result = ERenderViewResult::RequiredEnvironmentUnavailable};
		}

		const FDeferredDirectionalTimingQuerySink DeferredTimingSink =
			GetDeferredDirectionalTimingQuerySink();
		TScopedRendererGPUTimingQuery DeferredTiming(
			CommandList, DeferredTimingSink
		);
		const bool bDeferredRendered =
			DeferredDirectionalLightingRenderer.RenderProduction_RenderThread(
				CommandList, SceneColor, *DeferredParameters
			);
		DeferredTiming.Commit();
		if (!bDeferredRendered)
		{
			++Telemetry.View.Deferred.HybridDeferredUnavailableViews;
			return {};
		}
		const FRetainedOpaqueTimingQuerySink RetainedOpaqueTimingSink =
			GetRetainedOpaqueTimingQuerySink();
		TScopedRendererGPUTimingQuery RetainedOpaqueTiming(
			CommandList, RetainedOpaqueTimingSink
		);

		FRHIRenderPassInfo RetainedOpaque{};
		RetainedOpaque.RenderTargetLayout =
			RenderTargetLayouts::MakeHybridRetainedForward();
		RetainedOpaque.ColorRenderTargets[0] = SceneColor;
		RetainedOpaque.DepthStencilRenderTarget = Depth;
		CommandList.BeginRenderPass(
			RetainedOpaque, "HybridRetainedOpaqueRenderPass"
		);
		SetViewRect();
		for (const EMeshBasePass Pass : {
				 EMeshBasePass::Opaque, EMeshBasePass::Masked
			 })
		{
			const auto& StaticDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.StaticMeshes.Opaque : Inputs.Receiver.StaticMeshes.Masked;
			for (const FPreparedStaticMeshDraw& Draw : StaticDraws)
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						Inputs.Receiver.StaticMeshes,
						ResolvedFrame.Receiver.StaticMeshes, true
					);
				}
			const auto& SkeletalDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.SkeletalMeshes.Opaque : Inputs.Receiver.SkeletalMeshes.Masked;
			for (const FPreparedSkeletalMeshDraw& Draw : SkeletalDraws)
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						Inputs.Receiver.SkeletalMeshes,
						ResolvedFrame.Receiver.SkeletalMeshes, true
					);
				}
			const auto& TerrainDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.Terrains.Opaque : Inputs.Receiver.Terrains.Masked;
			for (const FPreparedTerrainDraw& Draw : TerrainDraws)
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					TerrainRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Draw,
						Inputs.Receiver.Terrains,
						ResolvedFrame.Receiver.Terrains,
						true
					);
				}
		}
		CommandList.EndRenderPass();
		RetainedOpaqueTiming.Commit();
		return {.Result = ERenderViewResult::Success};
	}

	auto FSceneRenderFeatureRecorders::RenderForwardScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneGeometryRecordInputs& Inputs,
		FRHITexture* RenderTarget
	) -> bool
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderScene");
		const FSceneView& View = Inputs.View;
		const uint32 Width = View.ViewportWidth;
		const uint32 Height = View.ViewportHeight;
		if (RenderTarget == nullptr || Width == 0 || Height == 0)
		{
			return false;
		}

		CommandList.SetViewport(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			0.0f,
			static_cast<float>(View.ViewportX + Width),
			static_cast<float>(View.ViewportY + Height),
			1.0f
		);
		CommandList.SetScissor(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(Width),
			static_cast<float>(Height)
		);

		if (Inputs.Environment != nullptr)
		{
			if (Inputs.Environment->Texture != nullptr)
			{
				if (!SkyBoxRenderer.DrawTexture_RenderThread(
						CommandList,
						View,
						Inputs.Environment->Texture,
						Inputs.Environment->SkyBox
					))
				{
					return false;
				}
			}
			else
			{
				SkyBoxRenderer.Draw_RenderThread(
					CommandList, View, Inputs.Environment->SkyBox
				);
			}
		}

		for (const EMeshBasePass Pass : {
				 EMeshBasePass::Opaque, EMeshBasePass::Masked
			 })
		{
			StaticMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.StaticMeshes,
				ResolvedFrame.Receiver.StaticMeshes
			);
			SkeletalMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.SkeletalMeshes,
				ResolvedFrame.Receiver.SkeletalMeshes
			);
			TerrainRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.Terrains,
				ResolvedFrame.Receiver.Terrains
			);
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 Inputs.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.StaticMeshes,
					ResolvedFrame.Receiver.StaticMeshes
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					Inputs.Receiver.Terrains.Translucent[Draw.DrawIndex],
					Inputs.Receiver.Terrains,
					ResolvedFrame.Receiver.Terrains
				);
		}
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.StaticMeshes
		);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.SkeletalMeshes
		);
		TerrainRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.Terrains);
		return true;
	}
} // namespace Durin
