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
	auto FDeferredDirectionalLightingGraphContributor::AddPasses(
		const FDeferredLightingGraphInputs& Inputs)
		-> FDeferredLightingGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto& RecordView = Inputs.View;
		const auto& Options = Inputs.Options;
		auto* DirectionalShadowTexture =
			Services.DirectionalShadowRenderer.GetTexture_RenderThread();
		auto* EnvironmentSampler = Inputs.EnvironmentSampler;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bWantsIsolatedDeferred = Inputs.bWantsIsolatedDeferred;
		const bool bWantsDeferredInputs = Inputs.bWantsDeferredInputs;
		const bool bWantsProductionDeferred = Inputs.bWantsProductionDeferred;
		const bool bHybridRetainedResourcesReady =
			Inputs.bHybridRetainedResourcesReady;
		auto& DeferredParameters = Inputs.DeferredParameters;
		auto& ProductionDeferredParameters = Inputs.ProductionDeferredParameters;
		FSceneFrameTopology Topology;
		Topology.bIsolatedDeferred = Inputs.bIsolated;
		Topology.AmbientOcclusionQuality = Inputs.AmbientOcclusion.Quality;
		struct {
			std::optional<FRenderGraphTextureHandle> DirectionalShadow;
			FRenderGraphTextureHandle SceneDepth;
			std::array<std::optional<FRenderGraphTextureHandle>, 4> GBuffer;
			std::array<std::optional<FRenderGraphTextureHandle>, 4>
				GroundTruthAmbientOcclusion;
			std::optional<FRenderGraphTextureHandle>
				ContactShadowVisibilityFragment;
			std::optional<FRenderGraphTextureHandle>
				ContactShadowVisibilityCompute;
			std::optional<FRenderGraphTextureHandle>
				VolumetricCloudShadowFragment;
			std::optional<FRenderGraphTextureHandle>
				VolumetricCloudShadowCompute;
			std::optional<FRenderGraphTextureHandle> DefaultWhite;
			std::optional<FRenderGraphTextureHandle> DefaultShadowArray;
			std::optional<FRenderGraphTextureHandle> EnvironmentIrradiance;
			std::optional<FRenderGraphTextureHandle> EnvironmentPrefiltered;
			std::optional<FRenderGraphTextureHandle> EnvironmentBrdfLut;
			FRHITexture* SelectedEnvironmentIrradiance = nullptr;
			FRHITexture* SelectedEnvironmentPrefiltered = nullptr;
			FRHITexture* SelectedEnvironmentBrdfLut = nullptr;
			std::optional<FRenderGraphTextureHandle> IsolatedDeferred;
		} GraphResources;
		GraphResources.DirectionalShadow = Inputs.DirectionalShadow.Shadow;
		GraphResources.GBuffer = Inputs.GBuffer.Textures;
		GraphResources.SceneDepth = Inputs.GBuffer.Depth;
		GraphResources.GroundTruthAmbientOcclusion = Inputs.AmbientOcclusion.Textures;
		GraphResources.ContactShadowVisibilityFragment = Inputs.ContactShadow.Fragment;
		GraphResources.ContactShadowVisibilityCompute = Inputs.ContactShadow.Compute;
		GraphResources.VolumetricCloudShadowFragment = Inputs.CloudShadow.Fragment;
		GraphResources.VolumetricCloudShadowCompute = Inputs.CloudShadow.Compute;
		GraphResources.DefaultWhite = Inputs.DefaultWhite;
		GraphResources.DefaultShadowArray = Inputs.DefaultShadowArray;
		GraphResources.EnvironmentIrradiance = Inputs.EnvironmentIrradiance;
		GraphResources.EnvironmentPrefiltered = Inputs.EnvironmentPrefiltered;
		GraphResources.EnvironmentBrdfLut = Inputs.EnvironmentBrdfLut;
		GraphResources.SelectedEnvironmentIrradiance = Inputs.SelectedEnvironmentIrradiance;
		GraphResources.SelectedEnvironmentPrefiltered = Inputs.SelectedEnvironmentPrefiltered;
		GraphResources.SelectedEnvironmentBrdfLut = Inputs.SelectedEnvironmentBrdfLut;
		struct {
			TSceneFrameGraphValue<FDirectionalShadowPassResult> DirectionalShadow;
			TSceneFrameGraphValue<FGBufferPassResult> GBuffer;
			TSceneFrameGraphValue<FGroundTruthAmbientOcclusionPassResult>
				AmbientOcclusion;
			TSceneFrameGraphValue<FContactShadowVisibilityPassResult>
				ContactShadowVisibility;
			TSceneFrameGraphValue<FVolumetricCloudShadowPassResult> CloudShadow;
			TSceneFrameGraphValue<FIsolatedDeferredPassResult>
				DeferredDirectionalLighting;
		} Channels;
		Channels.DirectionalShadow.Handle = Inputs.DirectionalShadow.Completion;
		Channels.GBuffer.Handle = Inputs.GBuffer.Completion;
		Channels.AmbientOcclusion.Handle = Inputs.AmbientOcclusion.Completion;
		Channels.ContactShadowVisibility.Handle = Inputs.ContactShadow.Completion;
		Channels.CloudShadow.Handle = Inputs.CloudShadow.Completion;
		Channels.DeferredDirectionalLighting.Handle = Graph.CreateValue<
			FIsolatedDeferredPassResult>("Scene.DeferredDirectionalLightingValue",
				"deferred-directional-lighting-result");
		if (Topology.bIsolatedDeferred)
			GraphResources.IsolatedDeferred = Graph.CreateTexture(
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DeferredDirectionalColor", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.ObservationTag = static_cast<uint32>(
						ERDGAllocationObservation::DeferredDirectional)},
				"Scene.DeferredDirectionalLighting.Isolated",
				ERHIAccess::GraphicsShaderRead);
		auto Parameters = Graph.AllocParameters<
			FDeferredDirectionalLightingPassParameters>();
		Parameters->DirectionalShadow = {.Value = Channels.DirectionalShadow.Handle};
		Parameters->GBufferCompletion = {.Value = Channels.GBuffer.Handle};
		Parameters->AmbientOcclusion = {.Value = Channels.AmbientOcclusion.Handle};
		Parameters->ContactShadow = {
			.Value = Channels.ContactShadowVisibility.Handle};
		Parameters->CloudShadow = {.Value = Channels.CloudShadow.Handle};
		Parameters->Completion = {
			.Value = Channels.DeferredDirectionalLighting.Handle};
		std::vector<FRenderGraphTextureHandle> DeclaredPersistentInputs;
		auto AssignRead = [&DeclaredPersistentInputs](auto& Parameter, const auto& Handle,
			FRHITexture* Physical) {
			if (!Handle || !Physical
				|| std::ranges::find(DeclaredPersistentInputs, *Handle)
					!= DeclaredPersistentInputs.end()) return;
			DeclaredPersistentInputs.push_back(*Handle);
			Parameter = FRenderGraphTextureParameter{*Handle,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()}};
		};
		AssignRead(Parameters->Resources.DirectionalShadow,
			GraphResources.DirectionalShadow, DirectionalShadowTexture);
		if (GraphResources.GBuffer[0])
		{
			for (uint32 Index = 0; Index < GraphResources.GBuffer.size(); ++Index)
				Parameters->Resources.GBuffer[Index] = {
					*GraphResources.GBuffer[Index],
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.SceneDepth = {GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
		}
		for (uint32 Index = 0;
			Index < GraphResources.GroundTruthAmbientOcclusion.size(); ++Index)
			if (GraphResources.GroundTruthAmbientOcclusion[Index])
				Parameters->Resources.AmbientOcclusion[Index] = {
					*GraphResources.GroundTruthAmbientOcclusion[Index],
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.ContactShadowVisibilityFragment)
			Parameters->Resources.ContactShadowFragment = {
				*GraphResources.ContactShadowVisibilityFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.ContactShadowVisibilityCompute)
			Parameters->Resources.ContactShadowCompute = {
				*GraphResources.ContactShadowVisibilityCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudShadowFragment)
			Parameters->Resources.CloudShadowFragment = {
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		if (GraphResources.VolumetricCloudShadowCompute)
			Parameters->Resources.CloudShadowCompute = {
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
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
		if (GraphResources.IsolatedDeferred)
			Parameters->Resources.IsolatedDeferredOutput = {
				*GraphResources.IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1}};
		(void)AddSceneFrameFeaturePass<FDeferredDirectionalLightingGraphContributor>(
			Graph, ERenderGraphPassType::Graphics, std::move(Parameters),
			[&Services, RecordView = &RecordView, Topology,
				&Options, &DeferredParameters, &ProductionDeferredParameters,
				Width, Height, bWantsDeferredInputs, bWantsIsolatedDeferred,
				bWantsProductionDeferred, bHybridRetainedResourcesReady,
				EnvironmentSampler,
				EnvironmentIrradiance = GraphResources.SelectedEnvironmentIrradiance,
				EnvironmentPrefiltered = GraphResources.SelectedEnvironmentPrefiltered,
				EnvironmentBrdfLut = GraphResources.SelectedEnvironmentBrdfLut](
				FRHICommandListImmediate& Commands,
				const FDeferredDirectionalLightingPassParameters& PassParameters,
				const FRenderGraphParameterResolver& Resolver) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (PassParameters.Resources.GBuffer[0])
					GBufferTargets = {
						.Material = Resolver.GetTexture(PassParameters.Resources.GBuffer[0]),
						.Normals = Resolver.GetTexture(PassParameters.Resources.GBuffer[1]),
						.Surface = Resolver.GetTexture(PassParameters.Resources.GBuffer[2]),
						.Emissive = Resolver.GetTexture(PassParameters.Resources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resolver.GetTexture(PassParameters.Resources.SceneDepth) : nullptr};
				std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
					AmbientOcclusionTargets;
				if (PassParameters.Resources.AmbientOcclusion[0])
					AmbientOcclusionTargets = {
						.Raw = Resolver.GetTexture(PassParameters.Resources.AmbientOcclusion[0]),
						.Scratch = Resolver.GetTexture(PassParameters.Resources.AmbientOcclusion[1]),
						.Selector = PassParameters.Resources.AmbientOcclusion[2]
							? Resolver.GetTexture(PassParameters.Resources.AmbientOcclusion[2])
							: nullptr,
						.Resolved = PassParameters.Resources.AmbientOcclusion[3]
							? Resolver.GetTexture(PassParameters.Resources.AmbientOcclusion[3])
							: nullptr,
						.Quality = Topology.AmbientOcclusionQuality};
				std::optional<FContactShadowVisibilityRenderer::FTargets>
					FragmentContactTargets;
				if (PassParameters.Resources.ContactShadowFragment)
					FragmentContactTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.ContactShadowFragment)};
				std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
					ComputeContactTargets;
				if (PassParameters.Resources.ContactShadowCompute)
					ComputeContactTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.ContactShadowCompute)};
				std::optional<FVolumetricCloudShadowRenderer::FTargets>
					FragmentCloudShadowTargets;
				if (PassParameters.Resources.CloudShadowFragment)
					FragmentCloudShadowTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.CloudShadowFragment)};
				std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
					ComputeCloudShadowTargets;
				if (PassParameters.Resources.CloudShadowCompute)
					ComputeCloudShadowTargets = {.Visibility = Resolver.GetTexture(
						PassParameters.Resources.CloudShadowCompute)};
				const auto& DirectionalShadowResult = Resolver.ReadValue(
					PassParameters.DirectionalShadow);
				const auto& GBufferResult = Resolver.ReadValue(
					PassParameters.GBufferCompletion);
				const auto& AmbientOcclusionResult = Resolver.ReadValue(
					PassParameters.AmbientOcclusion);
				const auto& ContactShadowResult = Resolver.ReadValue(
					PassParameters.ContactShadow);
				const auto& CloudShadowResult = Resolver.ReadValue(
					PassParameters.CloudShadow);
				auto& DeferredResult = Resolver.WriteValue(PassParameters.Completion);
				DeferredParameters = bWantsDeferredInputs
					? Services.Recorders.BuildDeferredParameters(
						*RecordView,
						PassParameters.Resources.EnvironmentIrradiance
							? Resolver.GetTexture(
								PassParameters.Resources.EnvironmentIrradiance)
							: EnvironmentIrradiance,
						PassParameters.Resources.EnvironmentPrefiltered
							? Resolver.GetTexture(
								PassParameters.Resources.EnvironmentPrefiltered)
							: EnvironmentPrefiltered,
						PassParameters.Resources.EnvironmentBrdfLut
							? Resolver.GetTexture(
								PassParameters.Resources.EnvironmentBrdfLut)
							: EnvironmentBrdfLut,
						EnvironmentSampler, DirectionalShadowResult,
						Resolver.GetTexture(PassParameters.Resources.DirectionalShadow),
						GBufferResult,
						GBufferTargets ? &*GBufferTargets : nullptr,
						AmbientOcclusionResult,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						ContactShadowResult,
						FragmentContactTargets ? &*FragmentContactTargets : nullptr,
						ComputeContactTargets ? &*ComputeContactTargets : nullptr,
						CloudShadowResult,
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
					if (PassParameters.Resources.IsolatedDeferredOutput)
						IsolatedTargets = {.Color = Resolver.GetColorAttachment(
							PassParameters.Resources.IsolatedDeferredOutput).Texture};
					DeferredResult = Services.Recorders.RenderIsolatedDeferred_RenderThread(
						Commands, IsolatedTargets ? &*IsolatedTargets : nullptr,
						*DeferredParameters, Options, Width, Height,
						bWantsIsolatedDeferred);
				}
				else if (bWantsIsolatedDeferred)
				{
					DeferredResult.Status = EScenePassStatus::Failed;
					++Services.Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
				}
				const bool bProductionResourcesReady =
					!bWantsProductionDeferred
					|| (GBufferResult.IsComplete()
						&& bHybridRetainedResourcesReady
						&& DeferredParameters.has_value());
				if (bWantsProductionDeferred && bProductionResourcesReady)
				{
					ProductionDeferredParameters = *DeferredParameters;
					ProductionDeferredParameters->DiagnosticMode = 0;
				}
			});
		return {.Completion = Channels.DeferredDirectionalLighting.Handle,
			.Isolated = GraphResources.IsolatedDeferred};
	}

	auto FSceneFrameFeatureRecorders::BuildDeferredParameters(
		const FSceneView& RenderView,
		FRHITexture* EnvironmentIrradiance,
		FRHITexture* EnvironmentPrefiltered,
		FRHITexture* EnvironmentBrdfLut,
		FRHISampler* EnvironmentSampler,
		const FDirectionalShadowPassResult& DirectionalShadow,
		FRHITexture* DirectionalShadowTexture,
		const FGBufferPassResult& GBuffer,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FGroundTruthAmbientOcclusionPassResult& AmbientOcclusion,
		const FGroundTruthAmbientOcclusionRenderer::FTargets*
			AmbientOcclusionTargets,
		const FContactShadowVisibilityPassResult& ContactShadow,
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
		FRHITexture* White =
			DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		const bool bAmbientOcclusionComplete = AmbientOcclusion.IsComplete()
			&& AmbientOcclusionTargets != nullptr;
		FRHITexture* ContactVisibility = White;
		bool bContactVisibilityComplete = false;
		if (ContactShadow.IsComplete())
		{
			if (ContactShadow.Route == EContactShadowVisibilityPassRoute::Compute
				&& ComputeContactTargets != nullptr)
			{
				ContactVisibility = ComputeContactTargets->Visibility;
				bContactVisibilityComplete = true;
			}
			else if (ContactShadow.Route == EContactShadowVisibilityPassRoute::Fragment
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
