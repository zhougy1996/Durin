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
	auto FAmbientOcclusionGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FSceneView& RecordView) -> void
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
		if (Topology.bGroundTruthAmbientOcclusion)
		{
			const bool bHalfResolution = Topology.AmbientOcclusionQuality
				== EGroundTruthAmbientOcclusionQuality::HalfResolution;
			const uint32 NativeWidth = bHalfResolution
				? FGroundTruthAmbientOcclusionRenderer::CalculateHalfExtent(Width)
				: Width;
			const uint32 NativeHeight = bHalfResolution
				? FGroundTruthAmbientOcclusionRenderer::CalculateHalfExtent(Height)
				: Height;
			const std::array Names{"Scene.AmbientOcclusion.Raw",
				"Scene.AmbientOcclusion.Scratch",
				"Scene.AmbientOcclusion.Selector",
				"Scene.AmbientOcclusion.Resolved"};
			for (uint32 Index = 0; Index < 2; ++Index)
				GraphResources.GroundTruthAmbientOcclusion[Index] =
					Graph.CreateTexture(Names[Index],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[Index],
								NativeWidth, NativeHeight, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								1.0f, 1.0f, 1.0f, 1.0f)),
							.BackingClass = std::string(GetSceneFrameBackingClassName(
								ESceneFrameBackingClass::AmbientOcclusion))},
						ERHIAccess::GraphicsShaderRead);
			if (bHalfResolution)
			{
				GraphResources.GroundTruthAmbientOcclusion[2] =
					Graph.CreateTexture(Names[2],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[2],
								NativeWidth, NativeHeight, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								0.0f, 0.0f, 0.0f, 0.0f)),
							.BackingClass = std::string(GetSceneFrameBackingClassName(
								ESceneFrameBackingClass::AmbientOcclusion))},
						ERHIAccess::GraphicsShaderRead);
				GraphResources.GroundTruthAmbientOcclusion[3] =
					Graph.CreateTexture(Names[3],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[3], Width,
								Height, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								1.0f, 1.0f, 1.0f, 1.0f)),
							.BackingClass = std::string(GetSceneFrameBackingClassName(
								ESceneFrameBackingClass::AmbientOcclusion))},
						ERHIAccess::GraphicsShaderRead);
			}
		}
		const auto AmbientOcclusionPass =
			AddSceneFrameFeaturePass<FAmbientOcclusionGraphContributor>(
				Graph, ERenderGraphPassType::Graphics,
			[&Services, &Channels, RecordView = &RecordView, &GraphResources, &Topology,
				&Options, Width, Height, bWantsGroundTruthAmbientOcclusion](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0]
					&& Topology.bGroundTruthAmbientOcclusion)
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
						.Quality = Topology.AmbientOcclusionQuality};
				Resources.WriteValue(Channels.AmbientOcclusion.Handle) =
					Services.Recorders.RenderGroundTruthAmbientOcclusion_RenderThread(
						Commands, *RecordView,
						GBufferTargets ? &*GBufferTargets : nullptr,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						SceneTargets, Options, Width, Height,
						bWantsGroundTruthAmbientOcclusion,
						Resources.ReadValue(Channels.GBuffer.Handle).IsComplete());
			});
		Graph.UseValue(AmbientOcclusionPass, GBufferValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseValue(AmbientOcclusionPass, AmbientOcclusionValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.GBuffer[0] && Topology.bGroundTruthAmbientOcclusion)
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(AmbientOcclusionPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(AmbientOcclusionPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
			for (const auto& Texture :
				GraphResources.GroundTruthAmbientOcclusion)
			{
				if (!Texture) continue;
				Graph.UseManagedTexture(AmbientOcclusionPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1},
					ERenderGraphUse::ReadWrite,
					ERHIAccess::GraphicsShaderRead,
					ERHIAccess::GraphicsShaderRead, true);
			}
		}
	}

	auto FSceneFrameFeatureRecorders::RenderGroundTruthAmbientOcclusion_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FGroundTruthAmbientOcclusionRenderer::FTargets*
			InAmbientOcclusionTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsGroundTruthAmbientOcclusion,
		bool bGBufferComplete
	) -> FGroundTruthAmbientOcclusionPassResult
	{
		FGroundTruthAmbientOcclusionPassResult Result;
		std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
			AmbientOcclusionTargetsStorage;
		if (InAmbientOcclusionTargets != nullptr)
			AmbientOcclusionTargetsStorage = *InAmbientOcclusionTargets;
		auto* AmbientOcclusionTargets = AmbientOcclusionTargetsStorage
			? &*AmbientOcclusionTargetsStorage : nullptr;
		if (bWantsGroundTruthAmbientOcclusion)
		{
			++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews;
			Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionRetainedBytes =
				GroundTruthAmbientOcclusionRenderer.GetRetainedTargetBytes_RenderThread();
			if (!bGBufferComplete || AmbientOcclusionTargets == nullptr)
			{
				Result.Status = EScenePassStatus::Failed;
				++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionUnavailableViews;
			}
			else
			{
				const FGroundTruthAmbientOcclusionFeatureTimingQuerySink
					FeatureTimingSink =
						GetGroundTruthAmbientOcclusionFeatureTimingQuerySink();
				TScopedRendererGPUTimingQuery FeatureTiming(
					CommandList, FeatureTimingSink
				);
				const FGroundTruthAmbientOcclusionTimingQuerySink TimingSink =
					GetGroundTruthAmbientOcclusionTimingQuerySink();
				bool bRendered = false;
				{
					TScopedRendererGPUTimingQuery RawTiming(CommandList, TimingSink);
					bRendered =
						GroundTruthAmbientOcclusionRenderer.RenderRaw_RenderThread(
							CommandList, *AmbientOcclusionTargets,
							GBufferTargets->Normals, GBufferTargets->Surface,
							SceneTargets.Depth, RenderView
						);
					if (bRendered)
						RawTiming.Commit();
				}
				if (bRendered)
				{
					const FGroundTruthAmbientOcclusionCaptureSink CaptureSink =
						GetGroundTruthAmbientOcclusionCaptureSink();
					if (CaptureSink != nullptr)
						CaptureSink(
							CommandList, AmbientOcclusionTargets->Raw, false
						);

					const FGroundTruthAmbientOcclusionFilterTimingQuerySink
						FilterTimingSink =
							GetGroundTruthAmbientOcclusionFilterTimingQuerySink();
					TScopedRendererGPUTimingQuery FilterTiming(
						CommandList, FilterTimingSink
					);
					const bool bFiltered =
						GroundTruthAmbientOcclusionRenderer.RenderFilter_RenderThread(
							CommandList, *AmbientOcclusionTargets,
							GBufferTargets->Normals, GBufferTargets->Surface,
							SceneTargets.Depth, RenderView
						);
					FilterTiming.End();
					const FGroundTruthAmbientOcclusionResolveTimingQuerySink
						ResolveTimingSink =
							GetGroundTruthAmbientOcclusionResolveTimingQuerySink();
					bool bResolved = false;
					if (bFiltered)
					{
						TScopedRendererGPUTimingQuery ResolveTiming(
							CommandList, ResolveTimingSink
						);
						bResolved =
							GroundTruthAmbientOcclusionRenderer.RenderResolve_RenderThread(
								CommandList, *AmbientOcclusionTargets,
								GBufferTargets->Normals, GBufferTargets->Surface,
								SceneTargets.Depth, RenderView
							);
						ResolveTiming.End();
						FeatureTiming.End();
						if (bResolved)
							ResolveTiming.Commit();
					}
					FeatureTiming.End();
					if (bResolved)
					{
						FeatureTiming.Commit();
						if (Options.GroundTruthAmbientOcclusionDebugMode
							== EGroundTruthAmbientOcclusionDebugMode::Raw)
						{
							std::swap(
								AmbientOcclusionTargets->Raw,
								AmbientOcclusionTargets->Scratch
							);
							const bool bRawDiagnosticRendered =
								GroundTruthAmbientOcclusionRenderer.RenderRaw_RenderThread(
									CommandList, *AmbientOcclusionTargets,
									GBufferTargets->Normals,
									GBufferTargets->Surface,
									SceneTargets.Depth, RenderView
								);
							std::swap(
								AmbientOcclusionTargets->Raw,
								AmbientOcclusionTargets->Scratch
							);
							Result.bRawDiagnosticUsesScratch =
								bRawDiagnosticRendered;
						}
						Result.Status = EScenePassStatus::Complete;
						Result.bHalfResolution =
							AmbientOcclusionTargets->Quality
							== EGroundTruthAmbientOcclusionQuality::HalfResolution;
						++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionEnabledViews;
						if (AmbientOcclusionTargets->Quality
							== EGroundTruthAmbientOcclusionQuality::HalfResolution)
							++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionHalfResolutionViews;
						else
							++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionFullResolutionViews;
						Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes =
							FGroundTruthAmbientOcclusionRenderer::
								CalculateTargetBytes(Width, Height, AmbientOcclusionTargets->Quality);
						if (Options.GroundTruthAmbientOcclusionDebugMode
							!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
						{
							++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionDebugViews;
						}
						FilterTiming.Commit();
						if (CaptureSink != nullptr)
							CaptureSink(
								CommandList, AmbientOcclusionTargets->Raw, true
							);
					}
					else if (!bFiltered)
					{
						Result.Status = EScenePassStatus::Failed;
						++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionFilterPassFailures;
					}
					else
					{
						Result.Status = EScenePassStatus::Failed;
						++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionResolvePassFailures;
					}
				}
				else
				{
					Result.Status = EScenePassStatus::Failed;
					++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionRawPassFailures;
				}
			}
		}
		return Result;
	}
} // namespace Durin
