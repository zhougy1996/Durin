#include "Renderers/BaseSceneRendering.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	#define DURIN_RESOURCE_MEMBER(Field, Wrapper, Kind, Use, Access, ...) \
		MakeRDGResourceParameterMemberMetadata<FParameters, \
			decltype(FParameters::Field), Wrapper>(#Field, offsetof(FParameters, Field), \
				Kind, ERDGResourceKind::Texture, \
				ERDGParameterRangeKind::TextureSubresource, Use, Access \
				__VA_OPT__(,) __VA_ARGS__)
	#define DURIN_TEXTURE(Field) DURIN_RESOURCE_MEMBER(Field, FRDGTextureParameter, \
		ERDGParameterMemberKind::Texture, ERDGUse::Read, \
		ERHIAccess::GraphicsShaderRead)
	#define DURIN_MANAGED_TEXTURE(Field, EntryAccess, Discard, ResultAccess) \
		DURIN_RESOURCE_MEMBER(Field, FRDGManagedTextureParameter, \
			ERDGParameterMemberKind::ManagedTexture, ERDGUse::ReadWrite, EntryAccess, \
			Discard, ERHIRenderTargetLoadAction::Load, \
			ERHIRenderTargetStoreAction::Store, true, ResultAccess)
	#define DURIN_DEFINE_METADATA(TypeName, ...) \
		auto TypeName::GetRDGParametersMetadata() -> const FRDGParametersMetadata* \
		{ using FParameters = TypeName; static const std::array Members = {__VA_ARGS__}; \
		static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>( \
			#TypeName, Members); return &Metadata; }

	DURIN_DEFINE_METADATA(FBaseScenePassResources,
		DURIN_TEXTURE(DirectionalShadow), DURIN_TEXTURE(DefaultWhite),
		DURIN_TEXTURE(DefaultShadowArray), DURIN_TEXTURE(EnvironmentIrradiance),
		DURIN_TEXTURE(EnvironmentPrefiltered), DURIN_TEXTURE(EnvironmentBrdfLut),
		DURIN_RESOURCE_MEMBER(SceneColorOutput, FRDGColorAttachmentParameter,
			ERDGParameterMemberKind::ManagedColorAttachment, ERDGUse::ReadWrite,
			ERHIAccess::ColorAttachmentReadWrite, true,
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store, true,
			ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(SceneDepthGraphicsToGraphics,
			ERHIAccess::GraphicsShaderRead, false, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(SceneDepthGraphicsToDepth,
			ERHIAccess::GraphicsShaderRead, false, ERHIAccess::DepthStencilReadWrite),
		DURIN_MANAGED_TEXTURE(SceneDepthDepthToGraphics,
			ERHIAccess::DepthStencilReadWrite, true, ERHIAccess::GraphicsShaderRead),
		DURIN_MANAGED_TEXTURE(SceneDepthDepthToDepth,
			ERHIAccess::DepthStencilReadWrite, true, ERHIAccess::DepthStencilReadWrite));

	DURIN_DEFINE_METADATA(FBaseScenePassParameters,
		MakeRDGValueParameterMemberMetadata<FParameters,
			decltype(FParameters::DeferredLighting), FIsolatedDeferredPassResult>(
				"DeferredLighting", offsetof(FParameters, DeferredLighting)),
		MakeRDGValueParameterMemberMetadata<FParameters,
			decltype(FParameters::Completion), FSceneColorPassResult>(
				"Completion", offsetof(FParameters, Completion)),
		MakeRDGNestedParameterMemberMetadata<FParameters,
			decltype(FParameters::Resources)>("Resources",
				offsetof(FParameters, Resources),
				FBaseScenePassResources::GetRDGParametersMetadata()));

	#undef DURIN_DEFINE_METADATA
	#undef DURIN_MANAGED_TEXTURE
	#undef DURIN_TEXTURE
	#undef DURIN_RESOURCE_MEMBER

	namespace
	{
		struct FBaseSceneRecorder final
		{
			FDeferredDirectionalLightingRenderer& DeferredDirectionalLightingRenderer;
			FStaticMeshRenderer& StaticMeshRenderer;
			FTerrainRenderer& TerrainRenderer;
			FSkeletalMeshRenderer& SkeletalMeshRenderer;
			FSkyBoxRenderer& SkyBoxRenderer;
			FSceneRenderTelemetry& Telemetry;
			FResolvedSceneResources& ResolvedSceneResources;

			auto RenderBaseScene_RenderThread(FRHICommandListImmediate&,
				const FSceneGeometryRecordInputs&, FRHITexture*, FRHITexture*,
				const FDeferredDirectionalLightingRenderer::FRenderParameters*)
				-> FSceneColorPassResult;
			auto RenderForwardScene_RenderThread(FRHICommandListImmediate&,
				const FSceneGeometryRecordInputs&, FRHITexture*) -> bool;
		};
	} // namespace

	auto FBaseSceneRendering::AddPasses(
		const FBaseSceneFeatureInputs& Inputs) -> FBaseSceneGraphOutput
	{
		auto& Graph = Inputs.Graph;
		FBaseSceneRecorder Recorder{Inputs.DeferredRenderer,
			Inputs.StaticMeshes, Inputs.Terrains, Inputs.SkeletalMeshes,
			Inputs.SkyBox, Inputs.Telemetry, Inputs.Resolved};
		const auto RecordInputs = Inputs.Record;
		const bool bRequiresDeferredOpaque =
			Inputs.DeferredFeature.HasPurpose(ESceneFeaturePurpose::Production);
		const bool bNeedsGBuffer = Inputs.GBufferFeature.IsEnabled();
		auto& ProductionDeferredParameters = Inputs.ProductionDeferredParameters;
		const auto BaseSceneCompletion = Graph.CreateValue<FSceneColorPassResult>(
			"Scene.BaseValue", "scene-color-result");
		auto Parameters = Graph.AllocParameters<FBaseScenePassParameters>();
		Parameters->DeferredLighting = {
			.Value = Inputs.Deferred.Completion};
		Parameters->Completion = {.Value = BaseSceneCompletion};
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
			Inputs.DirectionalShadow.Shadow,
			Inputs.DirectionalShadowRenderer.GetTexture_RenderThread());
		AssignRead(Parameters->Resources.DefaultWhite, Inputs.DefaultWhite,
			Inputs.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
		AssignRead(Parameters->Resources.DefaultShadowArray,
			Inputs.DefaultShadowArray,
			Inputs.DefaultTextures.GetArray_RenderThread());
		AssignRead(Parameters->Resources.EnvironmentIrradiance,
			Inputs.EnvironmentIrradiance,
			Inputs.SelectedEnvironmentIrradiance);
		AssignRead(Parameters->Resources.EnvironmentPrefiltered,
			Inputs.EnvironmentPrefiltered,
			Inputs.SelectedEnvironmentPrefiltered);
		AssignRead(Parameters->Resources.EnvironmentBrdfLut,
			Inputs.EnvironmentBrdfLut,
			Inputs.SelectedEnvironmentBrdfLut);
		Parameters->Resources.SceneColorOutput = {
			.Texture = Inputs.SceneColor,
			.Range = {ERHITextureAspect::Color, 0, 1, 0, 1}};
		FRDGManagedTextureParameter Depth{
			.Texture = Inputs.SceneDepth,
			.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		if (bNeedsGBuffer && bRequiresDeferredOpaque)
			Parameters->Resources.SceneDepthGraphicsToGraphics = Depth;
		else if (bNeedsGBuffer)
			Parameters->Resources.SceneDepthGraphicsToDepth = Depth;
		else if (bRequiresDeferredOpaque)
			Parameters->Resources.SceneDepthDepthToGraphics = Depth;
		else
			Parameters->Resources.SceneDepthDepthToDepth = Depth;
		(void)Graph.AddPass(Name, ERDGPassType::Graphics, std::move(Parameters),
			[Recorder, RecordInputs, &ProductionDeferredParameters](
				FRHICommandListImmediate& Commands,
				const FBaseScenePassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) mutable {
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
				Resolver.WriteValue(PassParameters.Completion) = Recorder.RenderBaseScene_RenderThread(
					Commands,
					RecordInputs,
					SceneTargets.Color, SceneTargets.Depth,
					ProductionDeferredParameters
						? &*ProductionDeferredParameters : nullptr);
				Timing.Commit();
			});
		return {.Completion = BaseSceneCompletion,
			.Color = Inputs.SceneColor, .Depth = Inputs.SceneDepth};
	}

	auto FBaseSceneRecorder::RenderBaseScene_RenderThread(
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
						CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						Inputs.Receiver.StaticMeshes,
						ResolvedSceneResources.Receiver.StaticMeshes, true
					);
				}
			const auto& SkeletalDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.SkeletalMeshes.Opaque : Inputs.Receiver.SkeletalMeshes.Masked;
			for (const FPreparedSkeletalMeshDraw& Draw : SkeletalDraws)
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						Inputs.Receiver.SkeletalMeshes,
						ResolvedSceneResources.Receiver.SkeletalMeshes, true
					);
				}
			const auto& TerrainDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.Terrains.Opaque : Inputs.Receiver.Terrains.Masked;
			for (const FPreparedTerrainDraw& Draw : TerrainDraws)
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					TerrainRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Draw,
						Inputs.Receiver.Terrains,
						ResolvedSceneResources.Receiver.Terrains,
						true
					);
				}
		}
		CommandList.EndRenderPass();
		RetainedOpaqueTiming.Commit();
		return {.Result = ERenderViewResult::Success};
	}

	auto FBaseSceneRecorder::RenderForwardScene_RenderThread(
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
				CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.StaticMeshes,
				ResolvedSceneResources.Receiver.StaticMeshes
			);
			SkeletalMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.SkeletalMeshes,
				ResolvedSceneResources.Receiver.SkeletalMeshes
			);
			TerrainRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.Terrains,
				ResolvedSceneResources.Receiver.Terrains
			);
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 Inputs.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.StaticMeshes,
					ResolvedSceneResources.Receiver.StaticMeshes
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.SkeletalMeshes,
					ResolvedSceneResources.Receiver.SkeletalMeshes
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					Inputs.Receiver.Terrains.Translucent[Draw.DrawIndex],
					Inputs.Receiver.Terrains,
					ResolvedSceneResources.Receiver.Terrains
				);
		}
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedSceneResources.Receiver.StaticMeshes
		);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedSceneResources.Receiver.SkeletalMeshes
		);
		TerrainRenderer.FinalizeExecution_RenderThread(
			ResolvedSceneResources.Receiver.Terrains);
		return true;
	}
} // namespace Durin
