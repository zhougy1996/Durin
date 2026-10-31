#include "Renderers/BaseSceneRendering.h"
#include "Resources/RenderTargetLayouts.h"
#include "Renderers/MeshRendererShared.h"
#include "Renderers/MeshPassProcessor.h"
#include "RDG/RDG.h"
#include "Renderers/SceneTextureGroupParameters.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/DirectionalShadowRenderer.h"
#include "Renderers/SkyBoxRenderer.h"
#include "Renderers/StaticMeshRenderer.h"
#include "Resources/DefaultTextureResources.h"

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
			auto RenderBootstrap(FRHICommandListImmediate&, const FSceneGeometryRecordInputs&) -> FSceneColorPassResult;
			auto RenderRetained(FRHICommandListImmediate&, const FSceneGeometryRecordInputs&) -> FSceneColorPassResult;
			auto RenderForwardScene_RenderThread(FRHICommandListImmediate&, const FSceneGeometryRecordInputs&, FRHITexture*) -> bool;
		};
	} // namespace

	auto AddBaseScenePasses(FRDGBuilder& Graph, const FBaseSceneFeatureInputs& Inputs) -> FBaseSceneGraphOutput
	{
		FBaseSceneRecorder Recorder{Inputs.DeferredRenderer, Inputs.StaticMeshes, Inputs.SkyBox, Inputs.Telemetry, Inputs.Resolved};
		auto RecordInputs = Inputs.Record;
		RecordInputs.bRequireEnvironmentTexture = RecordInputs.Environment && RecordInputs.Environment->Texture;
		const bool bHybrid = Inputs.DeferredFeature.HasPurpose(ESceneFeaturePurpose::Production);
		const auto Failure = Inputs.Failure;
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
		FRDGRasterPassOptions Raster;
		const auto& View = RecordInputs.View;
		Raster.ColorClearValues[0] = FClearValueBinding(View.ClearColor.r, View.ClearColor.g, View.ClearColor.b, View.ClearColor.a);
		Raster.DepthStencilClearValue = FClearValueBinding(View.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f, 0u);
		if (!bHybrid)
		{
			auto Parameters = MakeGeometryParameters(Completion, true, true);
			// Sky can alias a declared environment fallback; retain one range declaration.
			SceneTextureGroups::AssignSkyRead(Parameters->Resources, Sky, SkyTexture);
			(void)Graph.AddRasterPass(BaseScenePassName, std::move(Parameters), [Recorder, RecordInputs, Sky, Failure](FRHICommandListImmediate& Commands, const FBaseScenePassParameters& Pass, const FRDGParameterResolver& Resolver) mutable {
				TScopedRendererGPUTimingQuery Timing(Commands, GetSceneColorTimingQuerySink());
				auto Environment = SceneTextureGroups::ResolveSky(Resolver, Pass.Resources, Sky, RecordInputs.Environment);
				auto Record = RecordInputs;
				Record.Environment = Environment ? &*Environment : nullptr;
				const bool bRendered = Failure != ESceneRasterFailure::Forward && Recorder.RenderForwardScene_RenderThread(Commands, Record, Resolver.GetColorAttachment(Pass.Resources.SceneColorOutput).Texture);
				Resolver.WriteValue(Pass.Completion) = {.Result = bRendered ? ERenderViewResult::Success : ERenderViewResult::RequiredEnvironmentUnavailable};
				Timing.Commit();
			}, Raster);
		}
		else
		{
			const auto Bootstrap = Graph.CreateValue<FSceneColorPassResult>("Scene.Base.BootstrapValue", "scene-color-result");
			const auto Deferred = Graph.CreateValue<FSceneColorPassResult>("Scene.Base.DeferredValue", "scene-color-result");
			auto Timing = std::make_shared<FBaseSceneTiming>();
			auto Parameters = MakeGeometryParameters(Bootstrap, true, false);
			Parameters->GBufferCompletion = Inputs.Deferred.Inputs.GBufferCompletion;
			SceneTextureGroups::AssignSkyRead(Parameters->Resources, Sky, SkyTexture);
			(void)Graph.AddRasterPass(HybridBootstrapPassName, std::move(Parameters), [Recorder, RecordInputs, Sky, Timing, Failure](FRHICommandListImmediate& Commands, const FBaseScenePassParameters& Pass, const FRDGParameterResolver& Resolver) mutable {
				Timing->Begin(Commands);
				const auto* GBuffer = Resolver.ReadValue(Pass.GBufferCompletion);
				if (!GBuffer || !GBuffer->IsComplete())
				{
					++Recorder.Telemetry.View.Deferred.HybridDeferredUnavailableViews;
					Resolver.WriteValue(Pass.Completion) = {};
					return;
				}
				auto Environment = SceneTextureGroups::ResolveSky(Resolver, Pass.Resources, Sky, RecordInputs.Environment);
				auto Record = RecordInputs;
				Record.Environment = Environment ? &*Environment : nullptr;
				Resolver.WriteValue(Pass.Completion) = Failure == ESceneRasterFailure::HybridBootstrap
						? FSceneColorPassResult{.Result = ERenderViewResult::RequiredEnvironmentUnavailable}
						: Recorder.RenderBootstrap(Commands, Record);
			}, Raster);
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
			(void)Graph.AddRasterPass(RetainedForwardPassName, std::move(Retained), [Recorder, RecordInputs, Timing, Failure](FRHICommandListImmediate& Commands, const FBaseScenePassParameters& Pass, const FRDGParameterResolver& Resolver) mutable {
				auto& Result = Resolver.WriteValue(Pass.Completion);
				Result = *Resolver.ReadValue(Pass.Predecessor);
				if (Result.IsSuccess())
				{
					TScopedRendererGPUTimingQuery RetainedTiming(Commands, GetRetainedOpaqueTimingQuerySink());
					Result = Failure == ESceneRasterFailure::RetainedForward
							? FSceneColorPassResult{} : Recorder.RenderRetained(Commands, RecordInputs);
					RetainedTiming.Commit();
				}
				Timing->End(Commands);
			});
		}
		return {.Completion = Completion, .Color = Inputs.SceneColor, .Depth = Inputs.SceneDepth};
	}

	auto FBaseSceneRecorder::RenderBootstrap(FRHICommandListImmediate& CommandList, const FSceneGeometryRecordInputs& Inputs) -> FSceneColorPassResult
	{
		const auto& View = Inputs.View;
		check(CommandList.IsInsideRenderPass());
		SetViewRect(CommandList, View);
		bool bBootstrapRendered = true;
		if (Inputs.Environment)
		{
			const bool bSkyRendered = SkyBoxRenderer.DrawTexture_RenderThread(CommandList, View, Inputs.Environment->Texture, Inputs.Environment->SkyBox, true);
			bBootstrapRendered = bSkyRendered || !Inputs.bRequireEnvironmentTexture;
		}
		if (!bBootstrapRendered)
		{
			return {
				.Result = ERenderViewResult::RequiredEnvironmentUnavailable
			};
		}

		return {.Result = ERenderViewResult::Success};
	}

	auto FBaseSceneRecorder::RenderRetained(FRHICommandListImmediate& CommandList, const FSceneGeometryRecordInputs& Inputs) -> FSceneColorPassResult
	{
		const auto& View = Inputs.View;
		check(CommandList.IsInsideRenderPass());
		SetViewRect(CommandList, View);
		FMeshDrawCommandStateCache RetainedBindings;
		for (const EMeshBasePass Pass : {
				 EMeshBasePass::Opaque, EMeshBasePass::Masked
			 })
		{
			const auto& StaticDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.StaticMeshes.Opaque : Inputs.Receiver.StaticMeshes.Masked;
			for (const FPreparedStaticMeshDraw& Draw : StaticDraws)
				if (RendererPrivate::FPassProcessorManager::GetMeshPassProcessor(RendererPrivate::EMeshPipelinePass::HybridRetainedForward).IsMeshRelevant(
					Draw.Command->Material.PlanningPassIdentity, {}, Draw.Command->bSupportsGBuffer))
				{
					StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						Inputs.Receiver.StaticMeshes,
						ResolvedSceneResources.Receiver.StaticMeshes, true, &RetainedBindings
					);
				}
		}
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
		FMeshDrawCommandStateCache TranslucentBindings;
		for (const FPreparedStaticMeshDraw& Draw :
			 Inputs.Receiver.StaticMeshes.Translucent)
		{
			StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
				CommandList, View, ResolvedSceneResources.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, EMeshBasePass::Translucent, Draw,
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

namespace Durin::RendererPrivate
{
	namespace
	{
		template<typename T> auto FragmentShader() -> FMeshPassFragmentShader
		{
			FMeshPassFragmentShader Result{&T::StaticType(), +[](const FMeshMaterialShaderMap& Map) { return TMaterialShaderRef<T>(Map).GetRHIShader(false); }};
			if constexpr (std::is_base_of_v<FCompiledSurfaceMaterialShader, T>)
				Result.ResolveLayout = +[](const FMeshMaterialShaderMap& Map) -> const FCompiledSurfaceBindingLayout* { return &TMaterialShaderRef<T>(Map).GetShader()->GetSurfaceLayout(); };
			return Result;
		}
		template<EMeshPipelinePass Pass> class TBasePassMeshProcessor final : public FMeshPassProcessor
		{
		public:
			using FMeshPassProcessor::FMeshPassProcessor;
			auto AddMeshBatch(const FMeshPassProcessorBatch& Batch, const FMeshDrawCommandBuildInput& Input,
				FMeshDrawCommandBindingCache& Cache, FMeshPassDrawListContext& Context,
				std::shared_ptr<const FMeshDrawCommand> Existing) const -> FShaderCompletion override
			{ return BuildMeshDrawCommands(Batch, Input, Cache, Context, std::move(Existing)); }
			auto GetPass() const -> EMeshPipelinePass override { return Pass; }
			auto IsEnabled(const FSceneRenderPreparationConfig& Config) const -> bool override
			{ return Pass == EMeshPipelinePass::Forward ? Config.Settings.bForward : Config.Settings.bHybrid; }
			auto GetVertexFactoryPass(bool) const -> uint32 override { return MaterialMeshPassForward; }
			auto GetFragmentShader(FMaterialShaderBlendModeKey) const -> FMeshPassFragmentShader override
			{ return FragmentShader<FSurfaceFragmentShader>(); }
			auto GetPSOPrecacheVariants(const FSceneRenderPreparationConfig& Config, const FPSOPrecacheParams& Params) const -> std::vector<FMeshPSOPrecacheVariant> override
			{ return RasterVariants(Config, Params, 8); }
			auto DescribeMeshDraw(const FMeshPipelineState& State, FGraphicsPipelineStateInitializer::EPrimitiveTopology Topology,
				FMaterialShaderBlendModeKey Blend, bool) const -> FMeshPipelineDescription override
			{
				FMeshPipelineDescription Result{.Topology = Topology, .Rasterizer = State.Rasterizer, .Depth = State.Depth};
				if constexpr (Pass == EMeshPipelinePass::Forward) Result.RenderTargets = RenderTargetLayouts::MakeGraphSceneTargets();
				else if constexpr (Pass == EMeshPipelinePass::HybridRetainedForward) Result.RenderTargets = RenderTargetLayouts::MakeGraphHybridRetainedForward();
				else Result.RenderTargets = RenderTargetLayouts::MakeGraphSortedTranslucency();
				Result.ColorBlends[0] = State.ColorBlend;
				if (Blend == EMaterialBlendMode::Translucent) Result.ColorBlends[1].ColorWriteMask = ERHIColorWriteMask::None;
				return Result;
			}
		};
		template<EMeshPipelinePass Pass> auto CreateBasePassMeshProcessor(int32 PSOCollectorIndex) -> std::unique_ptr<FMeshPassProcessor>
		{ return std::make_unique<TBasePassMeshProcessor<Pass>>(PSOCollectorIndex); }
		template<EMeshPipelinePass Pass> auto CreateBasePSOCollector(ERHIFeatureLevel) -> std::unique_ptr<IPSOCollector>
		{
			constexpr auto Name = Pass == EMeshPipelinePass::Forward ? "Forward" : Pass == EMeshPipelinePass::HybridRetainedForward ? "HybridRetainedForward" : "SortedTranslucency";
			return CreateBasePassMeshProcessor<Pass>(FPSOCollectorCreateManager::GetIndex(EShadingPath::Deferred, Name));
		}
		const FRegisterPSOCollectorCreateFunction RegisterForwardPSO(&CreateBasePSOCollector<EMeshPipelinePass::Forward>, EShadingPath::Deferred, "Forward");
		const FRegisterPSOCollectorCreateFunction RegisterHybridPSO(&CreateBasePSOCollector<EMeshPipelinePass::HybridRetainedForward>, EShadingPath::Deferred, "HybridRetainedForward");
		const FRegisterPSOCollectorCreateFunction RegisterSortedPSO(&CreateBasePSOCollector<EMeshPipelinePass::SortedTranslucency>, EShadingPath::Deferred, "SortedTranslucency");
		const FRegisterPassProcessorCreateFunction RegisterForwardBasePass(&CreateBasePassMeshProcessor<EMeshPipelinePass::Forward>, EMeshPipelinePass::Forward);
		const FRegisterPassProcessorCreateFunction RegisterHybridRetainedForward(&CreateBasePassMeshProcessor<EMeshPipelinePass::HybridRetainedForward>, EMeshPipelinePass::HybridRetainedForward);
		const FRegisterPassProcessorCreateFunction RegisterSortedTranslucency(&CreateBasePassMeshProcessor<EMeshPipelinePass::SortedTranslucency>, EMeshPipelinePass::SortedTranslucency);
	}
}
