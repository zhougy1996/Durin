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
		const FAmbientOcclusionGraphInputs& Inputs)
		-> FAmbientOcclusionGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto& RecordView = Inputs.View;
		const auto& Options = Inputs.Options;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bWantsGroundTruthAmbientOcclusion = Inputs.bRequested;
		FSceneFrameTopology Topology;
		Topology.bGroundTruthAmbientOcclusion = Inputs.bEnabled;
		Topology.AmbientOcclusionQuality = Inputs.Quality;
		struct {
			FRDGTextureHandle SceneDepth;
			std::array<std::optional<FRDGTextureHandle>, 4> GBuffer;
			std::array<std::optional<FRDGTextureHandle>, 4>
				GroundTruthAmbientOcclusion;
		} GraphResources;
		GraphResources.GBuffer = Inputs.GBuffer.Textures;
		GraphResources.SceneDepth = Inputs.GBuffer.Depth;
		struct {
			TRDGValueHandle<FGBufferPassResult> GBuffer;
			TRDGValueHandle<FGroundTruthAmbientOcclusionPassResult>
				AmbientOcclusion;
		} Channels;
		Channels.GBuffer = Inputs.GBuffer.Completion;
		Channels.AmbientOcclusion = Graph.CreateValue<
			FGroundTruthAmbientOcclusionPassResult>(
				"Scene.AmbientOcclusionValue", "ambient-occlusion-result");
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
					Graph.CreateTexture(FRDGTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[Index],
								NativeWidth, NativeHeight, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								1.0f, 1.0f, 1.0f, 1.0f)),
							.ObservationTag = static_cast<uint32>(
								ERDGAllocationObservation::GroundTruthAmbientOcclusion)}, Names[Index],
						ERHIAccess::GraphicsShaderRead);
			if (bHalfResolution)
			{
				GraphResources.GroundTruthAmbientOcclusion[2] =
					Graph.CreateTexture(FRDGTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[2],
								NativeWidth, NativeHeight, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								0.0f, 0.0f, 0.0f, 0.0f)),
							.ObservationTag = static_cast<uint32>(
								ERDGAllocationObservation::GroundTruthAmbientOcclusion)}, Names[2],
						ERHIAccess::GraphicsShaderRead);
				GraphResources.GroundTruthAmbientOcclusion[3] =
					Graph.CreateTexture(FRDGTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[3], Width,
								Height, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								1.0f, 1.0f, 1.0f, 1.0f)),
							.ObservationTag = static_cast<uint32>(
								ERDGAllocationObservation::GroundTruthAmbientOcclusion)}, Names[3],
						ERHIAccess::GraphicsShaderRead);
			}
		}
		auto Parameters = Graph.AllocParameters<FAmbientOcclusionPassParameters>();
		Parameters->GBufferCompletion = {.Value = Channels.GBuffer};
		Parameters->Completion = {.Value = Channels.AmbientOcclusion};
		if (GraphResources.GBuffer[0] && Topology.bGroundTruthAmbientOcclusion)
		{
			for (uint32 Index = 0; Index < GraphResources.GBuffer.size(); ++Index)
				Parameters->Resources.GBuffer[Index] = {
					*GraphResources.GBuffer[Index],
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
			Parameters->Resources.SceneDepth = {GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
			for (uint32 Index = 0;
				Index < GraphResources.GroundTruthAmbientOcclusion.size(); ++Index)
				if (GraphResources.GroundTruthAmbientOcclusion[Index])
					Parameters->Resources.AmbientOcclusionManaged[Index] = {
						*GraphResources.GroundTruthAmbientOcclusion[Index],
						{ERHITextureAspect::Color, 0, 1, 0, 1}};
		}
		(void)AddSceneFrameFeaturePass<FAmbientOcclusionGraphContributor>(
			Graph, ERDGPassType::Graphics, std::move(Parameters),
			[&Services, RecordView = &RecordView, Topology,
				&Options, Width, Height, bWantsGroundTruthAmbientOcclusion](
				FRHICommandListImmediate& Commands,
				const FAmbientOcclusionPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (PassParameters.Resources.GBuffer[0]
					&& Topology.bGroundTruthAmbientOcclusion)
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
				if (PassParameters.Resources.AmbientOcclusionManaged[0])
					AmbientOcclusionTargets = {
						.Raw = Resolver.GetTexture(
							PassParameters.Resources.AmbientOcclusionManaged[0]),
						.Scratch = Resolver.GetTexture(
							PassParameters.Resources.AmbientOcclusionManaged[1]),
						.Selector = PassParameters.Resources.AmbientOcclusionManaged[2]
							? Resolver.GetTexture(
								PassParameters.Resources.AmbientOcclusionManaged[2])
							: nullptr,
						.Resolved = PassParameters.Resources.AmbientOcclusionManaged[3]
							? Resolver.GetTexture(
								PassParameters.Resources.AmbientOcclusionManaged[3])
							: nullptr,
						.Quality = Topology.AmbientOcclusionQuality};
				Resolver.WriteValue(PassParameters.Completion) =
					Services.Recorders.RenderGroundTruthAmbientOcclusion_RenderThread(
						Commands, *RecordView,
						GBufferTargets ? &*GBufferTargets : nullptr,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						SceneTargets, Options, Width, Height,
						bWantsGroundTruthAmbientOcclusion,
						Resolver.ReadValue(PassParameters.GBufferCompletion).IsComplete());
			});
		return {.Completion = Channels.AmbientOcclusion,
			.Textures = GraphResources.GroundTruthAmbientOcclusion,
			.Quality = Topology.AmbientOcclusionQuality};
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
				TransientTargets.GetObservedRetainedBytes_RenderThread(
					ERDGAllocationObservation::GroundTruthAmbientOcclusion);
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
