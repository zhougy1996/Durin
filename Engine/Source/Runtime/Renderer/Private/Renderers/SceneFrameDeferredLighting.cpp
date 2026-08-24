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
	auto FDeferredLightingGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneView& RecordView) -> void
	{
		auto& Graph = Context.Graph;
		auto& Services = Context.Services;
		const auto& View = Context.View;
		auto* OutputTarget = Context.OutputTarget;
		const auto& Options = Context.Options;
		auto& Requirements = Context.Topology;
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
		auto& ContactShadowValue = Channels.ContactShadow;
		auto& CloudShadowValue = Channels.CloudShadow;
		auto& DeferredValue = Channels.Deferred;
		auto& OpaqueSceneValue = Channels.OpaqueScene;
		auto& VolumetricCloudSpatialValue =
			Channels.VolumetricCloudSpatial;
		auto& VolumetricCloudValue = Channels.VolumetricCloud;
		auto& SceneColorValue = Channels.SceneColor;
		auto& PostProcessValue = Channels.PostProcess;
		auto& FinalOutputValue = Channels.FinalOutput;
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
				Services.EnvironmentLighting.GetIrradiance_RenderThread());
			Declare(GraphResources.EnvironmentPrefiltered,
				Services.EnvironmentLighting.GetPrefiltered_RenderThread());
			Declare(GraphResources.EnvironmentBrdfLut,
				Services.EnvironmentLighting.GetBrdfLut_RenderThread());
		};
		if (Requirements.bIsolatedDeferred)
			GraphResources.IsolatedDeferred = Graph.CreateTexture(
				"Scene.Deferred.Isolated",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DeferredDirectionalColor", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.BackingClass = std::string(GetSceneFrameBackingClassName(
						ESceneFrameBackingClass::Deferred))},
				ERHIAccess::GraphicsShaderRead);
		const auto DeferredPass =
			AddSceneFrameFeaturePass<FDeferredLightingGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordView = &RecordView, &GraphResources, &Requirements,
				&Options, &DeferredParameters, &ProductionDeferredParameters,
				Width, Height, bWantsDeferredInputs, bWantsIsolatedDeferred,
				bWantsProductionDeferred, bHybridRetainedResourcesReady](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0])
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
					AmbientOcclusionTargets;
				if (GraphResources.GroundTruthAmbientOcclusion[0])
					AmbientOcclusionTargets = {
						.Raw = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[0]),
						.Scratch = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[1]),
						.Selector = GraphResources.GroundTruthAmbientOcclusion[2]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[2])
							: nullptr,
						.Resolved = GraphResources.GroundTruthAmbientOcclusion[3]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[3])
							: nullptr,
						.Quality = Requirements.AmbientOcclusionQuality};
				std::optional<FContactShadowVisibilityRenderer::FTargets>
					FragmentContactTargets;
				if (GraphResources.ContactFragment)
					FragmentContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactFragment)};
				std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
					ComputeContactTargets;
				if (GraphResources.ContactCompute)
					ComputeContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactCompute)};
				std::optional<FVolumetricCloudShadowRenderer::FTargets>
					FragmentCloudShadowTargets;
				if (GraphResources.VolumetricCloudShadowFragment)
					FragmentCloudShadowTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowFragment)};
				std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
					ComputeCloudShadowTargets;
				if (GraphResources.VolumetricCloudShadowCompute)
					ComputeCloudShadowTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowCompute)};
				DeferredParameters = bWantsDeferredInputs
					? Services.Recorders.BuildDeferredParameters(
						*RecordView, Channels.DirectionalShadow.Result,
						GraphResources.DirectionalShadow
							? Resources.GetTexture(*GraphResources.DirectionalShadow)
							: nullptr,
						Channels.GBuffer.Result,
						GBufferTargets ? &*GBufferTargets : nullptr,
						Channels.AmbientOcclusion.Result,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						Channels.ContactShadow.Result,
						FragmentContactTargets ? &*FragmentContactTargets : nullptr,
						ComputeContactTargets ? &*ComputeContactTargets : nullptr,
						Channels.CloudShadow.Result,
						FragmentCloudShadowTargets
							? &*FragmentCloudShadowTargets : nullptr,
						ComputeCloudShadowTargets
							? &*ComputeCloudShadowTargets : nullptr,
						SceneTargets, Options)
					: std::nullopt;
				if (DeferredParameters)
				{
					std::optional<FDeferredDirectionalLightingRenderer::FTargets>
						IsolatedTargets;
					if (GraphResources.IsolatedDeferred)
						IsolatedTargets = {.Color = Resources.GetTexture(
							*GraphResources.IsolatedDeferred)};
					Channels.Deferred.Result = Services.Recorders.RenderIsolatedDeferred_RenderThread(
						Commands, IsolatedTargets ? &*IsolatedTargets : nullptr,
						*DeferredParameters, Options, Width, Height,
						bWantsIsolatedDeferred);
				}
				else if (bWantsIsolatedDeferred)
				{
					Channels.Deferred.Result.Status = EScenePassStatus::Failed;
					++Services.Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
				}
				const bool bProductionResourcesReady =
					!bWantsProductionDeferred
					|| (Channels.GBuffer.Result.IsComplete()
						&& bHybridRetainedResourcesReady
						&& DeferredParameters.has_value());
				if (bWantsProductionDeferred && bProductionResourcesReady)
				{
					ProductionDeferredParameters = *DeferredParameters;
					ProductionDeferredParameters->DiagnosticMode = 0;
				}
			});
		Graph.UseToken(DeferredPass, DirectionalShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, AmbientOcclusionValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, ContactShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, CloudShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, DeferredValue.Handle, ERenderGraphUse::Write);
		if (GraphResources.DirectionalShadow)
			Graph.UseTexture(DeferredPass, *GraphResources.DirectionalShadow,
				{ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.GBuffer[0])
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(DeferredPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(DeferredPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		}
		for (const auto& Texture : GraphResources.GroundTruthAmbientOcclusion)
		{
			if (!Texture) continue;
			Graph.UseTexture(DeferredPass, *Texture,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		}
		if (GraphResources.ContactFragment)
			Graph.UseTexture(DeferredPass, *GraphResources.ContactFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.ContactCompute)
			Graph.UseTexture(DeferredPass, *GraphResources.ContactCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudShadowFragment)
			Graph.UseTexture(DeferredPass,
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudShadowCompute)
			Graph.UseTexture(DeferredPass,
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		DeclarePersistentGraphicsInputs(DeferredPass);
		if (GraphResources.IsolatedDeferred)
			Graph.UseManagedColorAttachment(DeferredPass,
				*GraphResources.IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
	}

	auto FSceneFrameFeatureRecorders::BuildDeferredParameters(
		const FSceneView& RenderView,
		const FDirectionalShadowPassResult& DirectionalShadow,
		FRHITexture* DirectionalShadowTexture,
		const FGBufferPassResult& GBuffer,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FGroundTruthAmbientOcclusionPassResult& AmbientOcclusion,
		const FGroundTruthAmbientOcclusionRenderer::FTargets*
			AmbientOcclusionTargets,
		const FContactShadowPassResult& ContactShadow,
		const FContactShadowVisibilityRenderer::FTargets*
			FragmentContactTargets,
		const FContactShadowVisibilityRenderer::FComputeTargets*
			ComputeContactTargets,
		const FVolumetricCloudShadowPassResult& CloudShadow,
		const FVolumetricCloudShadowRenderer::FTargets*
			FragmentCloudShadowTargets,
		const FVolumetricCloudShadowRenderer::FComputeTargets*
			ComputeCloudShadowTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FSceneViewRenderOptions& Options
	) -> std::optional<
		FDeferredDirectionalLightingRenderer::FRenderParameters>
	{
		if (!GBuffer.IsComplete() || GBufferTargets == nullptr) return std::nullopt;
		FRHITexture* EnvironmentIrradiance =
			EnvironmentLighting.GetIrradiance_RenderThread();
		FRHITexture* EnvironmentPrefiltered =
			EnvironmentLighting.GetPrefiltered_RenderThread();
		FRHITexture* EnvironmentBrdfLut =
			EnvironmentLighting.GetBrdfLut_RenderThread();
		FRHISampler* EnvironmentSampler =
			EnvironmentLighting.GetSampler_RenderThread();
		if (EnvironmentIrradiance == nullptr
			|| EnvironmentPrefiltered == nullptr
			|| EnvironmentBrdfLut == nullptr
			|| EnvironmentSampler == nullptr)
		{
			EnvironmentIrradiance = DefaultTextures.GetCube_RenderThread();
			EnvironmentPrefiltered = DefaultTextures.GetCube_RenderThread();
			EnvironmentBrdfLut = DefaultTextures.Get_RenderThread(
				EDefaultTexture::Black);
			EnvironmentSampler = nullptr;
		}
		FRHITexture* White =
			DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		const bool bAmbientOcclusionComplete = AmbientOcclusion.IsComplete()
			&& AmbientOcclusionTargets != nullptr;
		FRHITexture* ContactVisibility = White;
		bool bContactVisibilityComplete = false;
		if (ContactShadow.IsComplete())
		{
			if (ContactShadow.Route == EContactShadowPassRoute::Compute
				&& ComputeContactTargets != nullptr)
			{
				ContactVisibility = ComputeContactTargets->Visibility;
				bContactVisibilityComplete = true;
			}
			else if (ContactShadow.Route == EContactShadowPassRoute::Fragment
				&& FragmentContactTargets != nullptr)
			{
				ContactVisibility = FragmentContactTargets->Visibility;
				bContactVisibilityComplete = true;
			}
		}
		FRHITexture* CloudShadowVisibility = White;
		bool bCloudShadowVisibilityComplete = false;
		if (CloudShadow.IsComplete())
		{
			if (CloudShadow.Route == EVolumetricCloudShadowPassRoute::Compute
				&& ComputeCloudShadowTargets != nullptr)
			{
				CloudShadowVisibility = ComputeCloudShadowTargets->Visibility;
				bCloudShadowVisibilityComplete = true;
			}
			else if (CloudShadow.Route == EVolumetricCloudShadowPassRoute::Fragment
				&& FragmentCloudShadowTargets != nullptr)
			{
				CloudShadowVisibility = FragmentCloudShadowTargets->Visibility;
				bCloudShadowVisibilityComplete = true;
			}
		}
		return FDeferredDirectionalLightingRenderer::FRenderParameters{
			.Material = GBufferTargets->Material,
			.Normals = GBufferTargets->Normals,
			.Surface = GBufferTargets->Surface,
			.Emissive = GBufferTargets->Emissive,
			.Depth = SceneTargets.Depth,
			.EnvironmentIrradiance = EnvironmentIrradiance,
			.EnvironmentPrefiltered = EnvironmentPrefiltered,
			.EnvironmentBrdfLut = EnvironmentBrdfLut,
			.EnvironmentSampler = EnvironmentSampler,
			.DirectionalShadowTexture = DirectionalShadow.IsComplete()
				&& DirectionalShadowTexture != nullptr
				? DirectionalShadowTexture
				: DefaultTextures.GetArray_RenderThread(),
			.DirectionalShadowSampler = DirectionalShadow.IsComplete()
				? DirectionalShadowRenderer.GetSampler_RenderThread() : nullptr,
			.GroundTruthAmbientOcclusionRaw = bAmbientOcclusionComplete
				? (AmbientOcclusion.bRawDiagnosticUsesScratch
					? AmbientOcclusionTargets->Scratch.GetReference()
					: AmbientOcclusionTargets->Raw.GetReference())
				: White,
			.GroundTruthAmbientOcclusionFiltered =
				bAmbientOcclusionComplete
					? AmbientOcclusionTargets->Raw.GetReference() : White,
			.GroundTruthAmbientOcclusionResolved =
				bAmbientOcclusionComplete
					? (AmbientOcclusion.bHalfResolution
						? AmbientOcclusionTargets->Resolved.GetReference()
						: AmbientOcclusionTargets->Raw.GetReference())
					: White,
			.GroundTruthAmbientOcclusionSelector =
				bAmbientOcclusionComplete && AmbientOcclusion.bHalfResolution
					? AmbientOcclusionTargets->Selector.GetReference() : White,
			.ContactVisibility = ContactVisibility,
			.VolumetricCloudVisibility = CloudShadowVisibility,
			.Lighting = ResolvedFrame.Lighting.UniformBuffer,
			.View = &RenderView,
			.DiagnosticMode = static_cast<uint32>(
				Options.DeferredDirectionalDebugMode),
			.bGroundTruthAmbientOcclusionEnabled = bAmbientOcclusionComplete,
			.bGroundTruthAmbientOcclusionHalfResolution =
				AmbientOcclusion.bHalfResolution,
			.bContactVisibilityEnabled = bContactVisibilityComplete,
			.bContactVisibilityDebug = ContactShadow.bDebug,
			.bVolumetricCloudVisibilityEnabled =
				bCloudShadowVisibilityComplete};
	}

	auto FSceneFrameFeatureRecorders::RenderIsolatedDeferred_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FDeferredDirectionalLightingRenderer::FTargets* Targets,
		const FDeferredDirectionalLightingRenderer::FRenderParameters& DeferredParameters,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsIsolatedDeferred
	) -> FIsolatedDeferredPassResult
	{
		FIsolatedDeferredPassResult Result;
		if (bWantsIsolatedDeferred)
		{
			Result.Status = EScenePassStatus::Failed;
			if (Targets == nullptr)
				++Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
			else
			{
				auto Parameters = DeferredParameters;
				Parameters.GroundTruthAmbientOcclusionDebugMode =
					static_cast<uint32>(
						Options.GroundTruthAmbientOcclusionDebugMode
					);
				const FDeferredDirectionalTimingQuerySink DeferredTimingSink =
					GetDeferredDirectionalTimingQuerySink();
				TScopedRendererGPUTimingQuery DeferredTiming(
					CommandList, DeferredTimingSink
				);
				const bool bRendered =
					DeferredDirectionalLightingRenderer.Render_RenderThread(
						CommandList, *Targets, Parameters
					);
				DeferredTiming.End();
				if (bRendered)
				{
					Result.Status = EScenePassStatus::Complete;
					++Telemetry.View.Deferred.DeferredDirectionalEnabledViews;
					Telemetry.View.Deferred.DeferredDirectionalOutputBytes =
						FDeferredDirectionalLightingRenderer::
							CalculateTargetBytes(Width, Height);
					if (Options.DeferredDirectionalDebugMode
						!= EDeferredDirectionalDebugMode::Disabled)
					{
						++Telemetry.View.Deferred.DeferredDirectionalDebugViews;
					}
					DeferredTiming.Commit();
					const FDeferredDirectionalCaptureSink CaptureSink =
						GetDeferredDirectionalCaptureSink();
					if (CaptureSink != nullptr)
						CaptureSink(CommandList, Targets->Color);
					if (Options.GroundTruthAmbientOcclusionDebugMode
						!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
					{
						Result.bOutputValid = true;
					}
				}
				else
				{
					++Telemetry.View.Deferred.DeferredDirectionalPassFailures;
				}
			}
		}
		return Result;
	}
} // namespace Durin
