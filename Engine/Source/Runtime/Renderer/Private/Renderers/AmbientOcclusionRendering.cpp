#include "Renderers/AmbientOcclusionRendering.h"
#include "Renderers/SceneTextureGroupParameters.h"
#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	auto FAmbientOcclusionPassResources::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FAmbientOcclusionPassResources;
		static const std::array Members = {
			MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::GBuffer)>("GBuffer", offsetof(FParameters, GBuffer)),
			MakeRDGTextureReadMetadata<FParameters,
				decltype(FParameters::SceneDepth)>("SceneDepth", offsetof(FParameters, SceneDepth)),
			MakeRDGManagedTextureMetadata<FParameters, decltype(FParameters::AmbientOcclusionManaged)>(
				"AmbientOcclusionManaged",
				offsetof(FParameters, AmbientOcclusionManaged),
				ERHIAccess::GraphicsShaderRead,
				true,
				ERHIAccess::GraphicsShaderRead)};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FAmbientOcclusionPassResources", Members);
		return &Metadata;
	}

	auto FAmbientOcclusionPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		using FParameters = FAmbientOcclusionPassParameters;
		static const std::array Members = {
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::GBufferCompletion), FGBufferPassResult>(
					"GBufferCompletion", offsetof(FParameters, GBufferCompletion)),
			MakeRDGValueParameterMemberMetadata<FParameters,
				decltype(FParameters::Completion),
				FGroundTruthAmbientOcclusionPassResult>("Completion",
					offsetof(FParameters, Completion)),
			MakeRDGNestedParameterMemberMetadata<FParameters,
				decltype(FParameters::Resources)>("Resources",
					offsetof(FParameters, Resources),
					FAmbientOcclusionPassResources::GetRDGParametersMetadata())};
		static const auto Metadata = MakeInlineRDGParametersMetadata<
			FParameters>("FAmbientOcclusionPassParameters", Members);
		return &Metadata;
	}

	namespace
	{
		auto RecordGroundTruthAmbientOcclusion(
			FRHICommandListImmediate& CommandList,
			const FSceneView& RenderView,
			const FGBufferRenderer::FTargets* GBufferTargets,
			FGroundTruthAmbientOcclusionRenderer::FTargets AmbientOcclusionTargets,
			const FPostProcessRenderer::FSceneTargets& SceneTargets,
			const FSceneViewRenderOptions& Options,
			uint32 Width,
			uint32 Height,
			bool bGBufferComplete,
			FRendererRDGAllocator& RDGAllocator,
			FGroundTruthAmbientOcclusionRenderer& GroundTruthAmbientOcclusionRenderer,
			FSceneRenderTelemetry& Telemetry
		) -> FGroundTruthAmbientOcclusionPassResult;
	} // namespace

	auto FAmbientOcclusionRendering::AddPasses(
		const FAmbientOcclusionFeatureInputs& Inputs)
		-> FAmbientOcclusionGraphOutput
	{
		if (!Inputs.Feature.IsEnabled()) return {};
		auto& Graph = Inputs.Graph;
		auto* Allocator = &Inputs.Allocator;
		auto* Renderer = &Inputs.Renderer;
		auto* Telemetry = &Inputs.Telemetry;
		const auto& RecordView = Inputs.View;
		const auto& Options = Inputs.Options;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const auto Quality = Inputs.Feature.Quality;
		const bool bHalfResolution = Quality == EGroundTruthAmbientOcclusionQuality::HalfResolution;
		const uint32 NativeWidth = bHalfResolution
			? FGroundTruthAmbientOcclusionRenderer::CalculateHalfExtent(Width) : Width;
		const uint32 NativeHeight = bHalfResolution
			? FGroundTruthAmbientOcclusionRenderer::CalculateHalfExtent(Height) : Height;
		const auto CreateTarget = [&](const char* Name, uint32 TargetWidth,
			uint32 TargetHeight, float ClearValue) {
			return Graph.CreateTexture(FRDGTextureDesc{.Texture =
				FRHITextureCreateDesc::Create2D(Name, TargetWidth, TargetHeight, EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy)
					.SetClearValue(FClearValueBinding(ClearValue, ClearValue, ClearValue, ClearValue)),
				.ObservationTag = static_cast<uint32>(ERDGAllocationObservation::GroundTruthAmbientOcclusion)},
				Name);
		};
		const auto AmbientOcclusionCompletion = Graph.CreateValue<FGroundTruthAmbientOcclusionPassResult>(
			"Scene.AmbientOcclusionValue", "ambient-occlusion-result");
		FAmbientOcclusionTextureHandles Textures{
			.Raw = CreateTarget("Scene.AmbientOcclusion.Raw", NativeWidth, NativeHeight, 1.0f),
			.Scratch = CreateTarget("Scene.AmbientOcclusion.Scratch", NativeWidth, NativeHeight, 1.0f)};
		if (bHalfResolution)
			Textures.HalfResolution = FAmbientOcclusionHalfResolutionTextures{
				.Selector = CreateTarget("Scene.AmbientOcclusion.Selector", NativeWidth, NativeHeight, 0.0f),
				.Resolved = CreateTarget("Scene.AmbientOcclusion.Resolved", Width, Height, 1.0f)};
		auto Parameters = Graph.AllocParameters<FAmbientOcclusionPassParameters>();
		Parameters->GBufferCompletion = {.Value = *Inputs.GBuffer.Completion};
		Parameters->Completion = {.Value = AmbientOcclusionCompletion};
		if (Inputs.GBuffer.Textures)
		{
			SceneTextureGroups::FillGBuffer(Inputs.GBuffer.Textures,
				Parameters->Resources.GBuffer);
			Parameters->Resources.SceneDepth = {Inputs.GBuffer.Depth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}};
		}
		SceneTextureGroups::FillAmbientOcclusion(Textures,
			Parameters->Resources.AmbientOcclusionManaged);
		(void)Graph.AddPass(Name, ERDGPassType::Graphics, std::move(Parameters),
			[Allocator, Renderer, Telemetry, RecordView = &RecordView, Quality,
				&Options, Width, Height](
				FRHICommandListImmediate& Commands,
				const FAmbientOcclusionPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				const auto GBufferTargets = SceneTextureGroups::ResolveGBuffer(
					Resolver, PassParameters.Resources.GBuffer);
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resolver.GetTexture(PassParameters.Resources.SceneDepth) : nullptr};
				const auto AmbientOcclusionTargets = SceneTextureGroups::ResolveAmbientOcclusion(
					Resolver, PassParameters.Resources.AmbientOcclusionManaged, Quality);
				Resolver.WriteValue(PassParameters.Completion) =
					RecordGroundTruthAmbientOcclusion(
						Commands, *RecordView,
						GBufferTargets ? &*GBufferTargets : nullptr,
						AmbientOcclusionTargets,
						SceneTargets, Options, Width, Height,
						Resolver.ReadValue(PassParameters.GBufferCompletion).IsComplete(),
						*Allocator, *Renderer, *Telemetry);
			});
		return {.Completion = AmbientOcclusionCompletion,
			.Textures = Textures,
			.Quality = Quality};
	}

	namespace
	{
	auto RecordGroundTruthAmbientOcclusion(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		const FGBufferRenderer::FTargets* GBufferTargets,
		FGroundTruthAmbientOcclusionRenderer::FTargets AmbientOcclusionTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bGBufferComplete,
		FRendererRDGAllocator& RDGAllocator,
		FGroundTruthAmbientOcclusionRenderer& GroundTruthAmbientOcclusionRenderer,
		FSceneRenderTelemetry& Telemetry
	) -> FGroundTruthAmbientOcclusionPassResult
	{
		FGroundTruthAmbientOcclusionPassResult Result;
		++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews;
		Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionRetainedBytes =
			RDGAllocator.GetObservedRetainedBytes_RenderThread(
				ERDGAllocationObservation::GroundTruthAmbientOcclusion);
		if (!bGBufferComplete || GBufferTargets == nullptr)
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
						CommandList, AmbientOcclusionTargets,
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
						CommandList, AmbientOcclusionTargets.Raw, false
					);

				const FGroundTruthAmbientOcclusionFilterTimingQuerySink
					FilterTimingSink =
						GetGroundTruthAmbientOcclusionFilterTimingQuerySink();
				TScopedRendererGPUTimingQuery FilterTiming(
					CommandList, FilterTimingSink
				);
				const bool bFiltered =
					GroundTruthAmbientOcclusionRenderer.RenderFilter_RenderThread(
						CommandList, AmbientOcclusionTargets,
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
							CommandList, AmbientOcclusionTargets,
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
							AmbientOcclusionTargets.Raw,
							AmbientOcclusionTargets.Scratch
						);
						const bool bRawDiagnosticRendered =
							GroundTruthAmbientOcclusionRenderer.RenderRaw_RenderThread(
								CommandList, AmbientOcclusionTargets,
								GBufferTargets->Normals,
								GBufferTargets->Surface,
								SceneTargets.Depth, RenderView
							);
						std::swap(
							AmbientOcclusionTargets.Raw,
							AmbientOcclusionTargets.Scratch
						);
						Result.bRawDiagnosticUsesScratch =
							bRawDiagnosticRendered;
					}
					Result.Status = EScenePassStatus::Complete;
					Result.bHalfResolution =
						AmbientOcclusionTargets.Quality
						== EGroundTruthAmbientOcclusionQuality::HalfResolution;
					++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionEnabledViews;
					if (AmbientOcclusionTargets.Quality
						== EGroundTruthAmbientOcclusionQuality::HalfResolution)
						++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionHalfResolutionViews;
					else
						++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionFullResolutionViews;
					Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionActiveBytes =
						FGroundTruthAmbientOcclusionRenderer::
							CalculateTargetBytes(Width, Height, AmbientOcclusionTargets.Quality);
					if (Options.GroundTruthAmbientOcclusionDebugMode
						!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
					{
						++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionDebugViews;
					}
					FilterTiming.Commit();
					if (CaptureSink != nullptr)
						CaptureSink(
							CommandList, AmbientOcclusionTargets.Raw, true
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

		return Result;
	}
	} // namespace
} // namespace Durin
