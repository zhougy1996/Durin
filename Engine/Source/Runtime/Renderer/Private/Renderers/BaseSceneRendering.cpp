#include "Renderers/BaseSceneRendering.h"
#include "RDG/RDG.h"
#include "Renderers/SceneTextureGroupParameters.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "SceneView.h"

namespace Durin
{
#define DURIN_TEXTURE(Field) \
	MakeRDGTextureReadMetadata<FParameters, decltype(FParameters::Field)>(#Field, offsetof(FParameters, Field))
#define DURIN_VALUE(Field, Type) \
	MakeRDGValueParameterMemberMetadata<FParameters, decltype(FParameters::Field), Type>(#Field, offsetof(FParameters, Field))
#define DURIN_NESTED(Field, Type) \
	MakeRDGNestedParameterMemberMetadata<FParameters, decltype(FParameters::Field)>(#Field, offsetof(FParameters, Field), Type::GetRDGParametersMetadata())
#define DURIN_DEFINE_METADATA(TypeName, ...)                                                           \
	auto TypeName::GetRDGParametersMetadata() -> const FRDGParametersMetadata*                         \
	{                                                                                                  \
		using FParameters = TypeName;                                                                  \
		static const std::array Members = {__VA_ARGS__};                                               \
		static const auto Metadata = MakeInlineRDGParametersMetadata<FParameters>(#TypeName, Members); \
		return &Metadata;                                                                              \
	}

	DURIN_DEFINE_METADATA(FBaseScenePassResources, DURIN_TEXTURE(DirectionalShadow), DURIN_TEXTURE(DefaultWhite), DURIN_TEXTURE(DefaultShadowArray), DURIN_TEXTURE(EnvironmentIrradiance), DURIN_TEXTURE(EnvironmentPrefiltered), DURIN_TEXTURE(EnvironmentBrdfLut), DURIN_TEXTURE(EnvironmentSky), MakeRDGColorAttachmentBindingMetadata<FParameters, decltype(FParameters::SceneColorOutput)>("SceneColorOutput", offsetof(FParameters, SceneColorOutput)), MakeRDGDepthStencilAttachmentBindingMetadata<FParameters, decltype(FParameters::SceneDepthOutput)>("SceneDepthOutput", offsetof(FParameters, SceneDepthOutput)));
	DURIN_DEFINE_METADATA(FBaseScenePassParameters, DURIN_VALUE(GBufferCompletion, FGBufferPassResult), DURIN_VALUE(Predecessor, FSceneColorPassResult), DURIN_VALUE(Completion, FSceneColorPassResult), DURIN_NESTED(Resources, FBaseScenePassResources));
	DURIN_DEFINE_METADATA(FProductionDeferredPassParameters, DURIN_NESTED(Inputs, FDeferredLightingInputParameters), DURIN_VALUE(Predecessor, FSceneColorPassResult), DURIN_VALUE(Completion, FSceneColorPassResult), MakeRDGColorAttachmentBindingMetadata<FParameters, decltype(FParameters::SceneColorOutput)>("SceneColorOutput", offsetof(FParameters, SceneColorOutput)));
#undef DURIN_DEFINE_METADATA
#undef DURIN_NESTED
#undef DURIN_VALUE
#undef DURIN_TEXTURE

	namespace
	{
		auto SetViewRect(FRHICommandListImmediate& Commands, const FSceneView& View) -> void
		{
			Commands.SetViewport(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY), 0.0f, static_cast<float>(View.ViewportX + View.ViewportWidth), static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
			Commands.SetScissor(static_cast<float>(View.ViewportX), static_cast<float>(View.ViewportY), static_cast<float>(View.ViewportWidth), static_cast<float>(View.ViewportHeight));
		}

		struct FGraphRasterPass final
		{
			FRHIRenderPassInfo Pass;
			FRDGNativeAttachmentBinding Color;
			FRDGNativeAttachmentBinding Depth;
		};

		struct FBaseSceneTiming final
		{
			FGPUTimingQueryRHIRef Query;
			FSceneColorTimingQuerySink Sink = GetSceneColorTimingQuerySink();
			auto Begin(FRHICommandListImmediate& Commands) -> void
			{
				if (!Sink || !GDynamicRHI) return;
				Query = GDynamicRHI->RHICreateGPUTimingQuery();
				if (Query) Commands.BeginGPUTimingQuery(Query);
			}
			auto End(FRHICommandListImmediate& Commands) -> void
			{
				if (!Query) return;
				Commands.EndGPUTimingQuery(Query);
				Sink(Query);
				Query = nullptr;
			}
		};

		struct FBaseSceneRecorder final
		{
			FDeferredDirectionalLightingRenderer& DeferredDirectionalLightingRenderer;
			FStaticMeshRenderer& StaticMeshRenderer;
			FSkyBoxRenderer& SkyBoxRenderer;
			FSceneRenderTelemetry& Telemetry;
			FResolvedSceneResources& ResolvedSceneResources;
			auto RenderBootstrap(FRHICommandListImmediate&, const FSceneGeometryRecordInputs&, const FRHIRenderPassInfo&) -> FSceneColorPassResult;
			auto RenderRetained(FRHICommandListImmediate&, const FSceneGeometryRecordInputs&, const FRHIRenderPassInfo&) -> FSceneColorPassResult;
			auto RenderForwardScene_RenderThread(FRHICommandListImmediate&, const FSceneGeometryRecordInputs&, FRHITexture*) -> bool;
		};
	} // namespace

	auto AddBaseScenePasses(FRDGBuilder& Graph, const FBaseSceneFeatureInputs& Inputs) -> FBaseSceneGraphOutput
	{
		FBaseSceneRecorder Recorder{Inputs.DeferredRenderer, Inputs.StaticMeshes, Inputs.SkyBox, Inputs.Telemetry, Inputs.Resolved};
		auto RecordInputs = Inputs.Record;
		RecordInputs.bRequireEnvironmentTexture = RecordInputs.Environment && RecordInputs.Environment->Texture;
		const bool bHybrid = Inputs.DeferredFeature.HasPurpose(ESceneFeaturePurpose::Production);
		const auto Failure = GetRendererQualificationPolicy().RasterFailure;
		const auto Completion = Graph.CreateValue<FSceneColorPassResult>("Scene.BaseValue", "scene-color-result");
		std::optional<FRDGTextureHandle> Sky;
		FRHITexture* SkyTexture = nullptr;
		if (Inputs.Record.Environment)
		{
			SkyTexture = Inputs.Record.Environment->Texture;
			if (!SkyTexture && Inputs.Record.Environment->SkyBox.TextureReference)
				SkyTexture = Inputs.Record.Environment->SkyBox.TextureReference->GetReferencedTexture_RenderThread();
			if (!SkyTexture) SkyTexture = Inputs.DefaultTextures.GetCube_RenderThread();
			if (SkyTexture)
				Sky = Graph.RegisterExternalTexture(FTextureRHIRef(SkyTexture), "Scene.Environment.Sky", ERHIAccess::GraphicsShaderRead, ERHIAccess::GraphicsShaderRead);
		}
		const auto MakeGeometryParameters = [&](TRDGValueHandle<FSceneColorPassResult> Result, bool bClearColor, bool bClearDepth) {
			auto Parameters = Graph.AllocParameters<FBaseScenePassParameters>();
			Parameters->Completion = {.Value = Result};
			Parameters->Resources.SceneColorOutput = {Inputs.SceneColor, {ERHITextureAspect::Color, 0, 1, 0, 1}, bClearColor ? ERHIRenderTargetLoadAction::Clear : ERHIRenderTargetLoadAction::Load};
			Parameters->Resources.SceneDepthOutput = {Inputs.SceneDepth, {ERHITextureAspect::Depth, 0, 1, 0, 1}, bClearDepth ? ERHIRenderTargetLoadAction::Clear : ERHIRenderTargetLoadAction::Load};
			if (!bHybrid || !bClearColor)
			{
				SceneTextureGroups::FPersistentTextureReads Reads;
				Reads.Assign(Parameters->Resources.DirectionalShadow, Inputs.DirectionalShadow.Shadow, Inputs.DirectionalShadowRenderer.GetTexture_RenderThread());
				Reads.Assign(Parameters->Resources.DefaultWhite, Inputs.DefaultWhite, Inputs.DefaultTextures.Get_RenderThread(EDefaultTexture::White));
				Reads.Assign(Parameters->Resources.DefaultShadowArray, Inputs.DefaultShadowArray, Inputs.DefaultTextures.GetArray_RenderThread());
				Reads.Assign(Parameters->Resources.EnvironmentIrradiance, Inputs.Environment.Irradiance, Inputs.Environment.SelectedIrradiance);
				Reads.Assign(Parameters->Resources.EnvironmentPrefiltered, Inputs.Environment.Prefiltered, Inputs.Environment.SelectedPrefiltered);
				Reads.Assign(Parameters->Resources.EnvironmentBrdfLut, Inputs.Environment.BrdfLut, Inputs.Environment.SelectedBrdfLut);
			}
			return Parameters;
		};
		const auto MakePassInfo = [](const FBaseScenePassParameters& Parameters, const FRDGParameterResolver& Resolver, const FSceneView& View) {
			FGraphRasterPass Result;
			Result.Color = MakeRDGNativeAttachmentBinding(Resolver.GetColorAttachment(Parameters.Resources.SceneColorOutput));
			Result.Depth = MakeRDGNativeAttachmentBinding(Resolver.GetDepthStencilAttachment(Parameters.Resources.SceneDepthOutput));
			auto& Pass = Result.Pass;
			Pass.RenderTargetLayout.NumColorRenderTargets = 1;
			Pass.RenderTargetLayout.ColorAttachments[0].RenderTarget = Result.Color.Layout;
			Pass.RenderTargetLayout.bHasDepthStencil = true;
			Pass.RenderTargetLayout.DepthStencilAttachment = Result.Depth.Layout;
			Result.Color.BindColor(Pass, 0);
			Result.Depth.BindDepthStencil(Pass);
			Pass.ColorClearValues[0] = FClearValueBinding(View.ClearColor.r, View.ClearColor.g, View.ClearColor.b, View.ClearColor.a);
			Pass.DepthStencilClearValue = FClearValueBinding(View.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f, 0u);
			return Result;
		};
		if (!bHybrid)
		{
			auto Parameters = MakeGeometryParameters(Completion, true, true);
			// Sky can alias a declared environment fallback; retain one range declaration.
			SceneTextureGroups::AssignSkyRead(Parameters->Resources, Sky, SkyTexture);
			(void)Graph.AddPass(BaseScenePassName, ERDGPassType::Graphics, std::move(Parameters), [Recorder, RecordInputs, MakePassInfo, Sky, Failure](FRHICommandListImmediate& Commands, const FBaseScenePassParameters& Pass, const FRDGParameterResolver& Resolver) mutable {
				TScopedRendererGPUTimingQuery Timing(Commands, GetSceneColorTimingQuerySink());
				auto Environment = SceneTextureGroups::ResolveSky(Resolver, Pass.Resources, Sky, RecordInputs.Environment);
				auto Record = RecordInputs;
				Record.Environment = Environment ? &*Environment : nullptr;
				const auto Native = MakePassInfo(Pass, Resolver, RecordInputs.View);
				Commands.BeginRenderPass(Native.Pass, "SceneColorRenderPass");
				const bool bRendered = Failure != ESceneRasterFailure::Forward && Recorder.RenderForwardScene_RenderThread(Commands, Record, Resolver.GetColorAttachment(Pass.Resources.SceneColorOutput).Texture);
				Commands.EndRenderPass();
				Resolver.WriteValue(Pass.Completion) = {.Result = bRendered ? ERenderViewResult::Success : ERenderViewResult::RequiredEnvironmentUnavailable};
				Timing.Commit();
			});
		}
		else
		{
			const auto Bootstrap = Graph.CreateValue<FSceneColorPassResult>("Scene.Base.BootstrapValue", "scene-color-result");
			const auto Deferred = Graph.CreateValue<FSceneColorPassResult>("Scene.Base.DeferredValue", "scene-color-result");
			auto Timing = std::make_shared<FBaseSceneTiming>();
			auto Parameters = MakeGeometryParameters(Bootstrap, true, false);
			Parameters->GBufferCompletion = Inputs.Deferred.Inputs.GBufferCompletion;
			SceneTextureGroups::AssignSkyRead(Parameters->Resources, Sky, SkyTexture);
			(void)Graph.AddPass(HybridBootstrapPassName, ERDGPassType::Graphics, std::move(Parameters), [Recorder, RecordInputs, MakePassInfo, Sky, Timing, Failure, Policy = Inputs.Deferred.Policy](FRHICommandListImmediate& Commands, const FBaseScenePassParameters& Pass, const FRDGParameterResolver& Resolver) mutable {
				Timing->Begin(Commands);
				const auto* GBuffer = Resolver.ReadValue(Pass.GBufferCompletion);
				if (!GBuffer || !GBuffer->IsComplete() || !Policy.bRetainedResourcesReady)
				{
					++Recorder.Telemetry.View.Deferred.HybridDeferredUnavailableViews;
					Resolver.WriteValue(Pass.Completion) = {};
					return;
				}
				auto Environment = SceneTextureGroups::ResolveSky(Resolver, Pass.Resources, Sky, RecordInputs.Environment);
				auto Record = RecordInputs;
				Record.Environment = Environment ? &*Environment : nullptr;
				const auto Native = MakePassInfo(Pass, Resolver, RecordInputs.View);
				Resolver.WriteValue(Pass.Completion) = Failure == ESceneRasterFailure::HybridBootstrap
						? FSceneColorPassResult{.Result = ERenderViewResult::RequiredEnvironmentUnavailable}
						: Recorder.RenderBootstrap(Commands, Record, Native.Pass);
			});
			auto Lighting = Graph.AllocParameters<FProductionDeferredPassParameters>();
			Lighting->Inputs = Inputs.Deferred.Inputs;
			Lighting->Predecessor = {.Value = Bootstrap};
			Lighting->Completion = {.Value = Deferred};
			Lighting->SceneColorOutput = {Inputs.SceneColor, {ERHITextureAspect::Color, 0, 1, 0, 1}};
			(void)Graph.AddPass(ProductionDeferredPassName, ERDGPassType::Graphics, std::move(Lighting), [Recorder, RecordInputs, Failure, Policy = Inputs.Deferred.Policy](FRHICommandListImmediate& Commands, const FProductionDeferredPassParameters& Pass, const FRDGParameterResolver& Resolver) mutable {
				auto& Result = Resolver.WriteValue(Pass.Completion);
				Result = Resolver.ReadValue(Pass.Predecessor);
				if (!Result.IsSuccess()) return;
				const auto Physical = ResolveDeferredLightingParameters(Resolver, Pass.Inputs, Policy, RecordInputs.View, FSceneViewRenderOptions{}, Recorder.ResolvedSceneResources.Lighting.UniformBuffer);
				FRHIRenderPassInfo Native;
				const auto ColorBinding = MakeRDGNativeAttachmentBinding(Resolver.GetColorAttachment(Pass.SceneColorOutput));
				Native.RenderTargetLayout.NumColorRenderTargets = 1;
				Native.RenderTargetLayout.ColorAttachments[0].RenderTarget = ColorBinding.Layout;
				ColorBinding.BindColor(Native, 0);
				TScopedRendererGPUTimingQuery Timing(Commands, GetDeferredDirectionalTimingQuerySink());
				const bool bRendered = Failure != ESceneRasterFailure::ProductionDeferred && Physical && Recorder.DeferredDirectionalLightingRenderer.RenderProduction_RenderThread(Commands, Native, *Physical);
				Timing.Commit();
				if (!bRendered)
				{
					++Recorder.Telemetry.View.Deferred.HybridDeferredUnavailableViews;
					Result = {};
				}
			});
			auto Retained = MakeGeometryParameters(Completion, false, false);
			Retained->Predecessor = TRDGValueRead<FSceneColorPassResult>{Deferred};
			(void)Graph.AddPass(RetainedForwardPassName, ERDGPassType::Graphics, std::move(Retained), [Recorder, RecordInputs, MakePassInfo, Timing, Failure](FRHICommandListImmediate& Commands, const FBaseScenePassParameters& Pass, const FRDGParameterResolver& Resolver) mutable {
				auto& Result = Resolver.WriteValue(Pass.Completion);
				Result = *Resolver.ReadValue(Pass.Predecessor);
				if (Result.IsSuccess())
				{
					TScopedRendererGPUTimingQuery RetainedTiming(Commands, GetRetainedOpaqueTimingQuerySink());
					const auto Native = MakePassInfo(Pass, Resolver, RecordInputs.View);
					Result = Failure == ESceneRasterFailure::RetainedForward
							? FSceneColorPassResult{} : Recorder.RenderRetained(Commands, RecordInputs, Native.Pass);
					RetainedTiming.Commit();
				}
				Timing->End(Commands);
			});
		}
		return {.Completion = Completion, .Color = Inputs.SceneColor, .Depth = Inputs.SceneDepth};
	}

	auto FBaseSceneRecorder::RenderBootstrap(FRHICommandListImmediate& CommandList, const FSceneGeometryRecordInputs& Inputs, const FRHIRenderPassInfo& Pass) -> FSceneColorPassResult
	{
		const auto& View = Inputs.View;
		CommandList.BeginRenderPass(Pass, "HybridSceneBootstrapRenderPass");
		SetViewRect(CommandList, View);
		bool bBootstrapRendered = true;
		if (Inputs.Environment)
		{
			const bool bSkyRendered = SkyBoxRenderer.DrawTexture_RenderThread(CommandList, View, Inputs.Environment->Texture, Inputs.Environment->SkyBox, true);
			bBootstrapRendered = bSkyRendered || !Inputs.bRequireEnvironmentTexture;
		}
		CommandList.EndRenderPass();
		if (!bBootstrapRendered)
		{
			return {
				.Result = ERenderViewResult::RequiredEnvironmentUnavailable
			};
		}

		return {.Result = ERenderViewResult::Success};
	}

	auto FBaseSceneRecorder::RenderRetained(FRHICommandListImmediate& CommandList, const FSceneGeometryRecordInputs& Inputs, const FRHIRenderPassInfo& Pass) -> FSceneColorPassResult
	{
		const auto& View = Inputs.View;
		CommandList.BeginRenderPass(Pass, "HybridRetainedOpaqueRenderPass");
		SetViewRect(CommandList, View);
		FMeshDrawBindingGroup RetainedBindings;
		for (const EMeshBasePass Pass : {
				 EMeshBasePass::Opaque, EMeshBasePass::Masked
			 })
		{
			const auto& StaticDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.StaticMeshes.Opaque : Inputs.Receiver.StaticMeshes.Masked;
			for (const FPreparedStaticMeshDraw& Draw : StaticDraws)
				if (!Draw.Command->bSupportsGBuffer || Draw.Command->Material.PlanningPassIdentity.ShaderMap.ShadingModel != EMaterialShadingModel::Lit)
				{
					StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						Inputs.Receiver.StaticMeshes,
						ResolvedSceneResources.Receiver.StaticMeshes, true, &RetainedBindings
					);
				}
		}
		CommandList.EndRenderPass();
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

		if (Inputs.Environment)
		{
			const bool bSkyRendered = SkyBoxRenderer.DrawTexture_RenderThread(CommandList, View, Inputs.Environment->Texture, Inputs.Environment->SkyBox);
			if (!bSkyRendered && Inputs.bRequireEnvironmentTexture) return false;
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
		}
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
					ResolvedSceneResources.Receiver.StaticMeshes, false, &TranslucentBindings
				);
		}
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedSceneResources.Receiver.StaticMeshes
		);
		return true;
	}
} // namespace Durin
