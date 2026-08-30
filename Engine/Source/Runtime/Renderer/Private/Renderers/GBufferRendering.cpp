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
	auto FGBufferPassParameters::GetRDGParametersMetadata()
		-> const FRDGParametersMetadata*
	{
		static const std::array Members{
			MakeRDGValueParameterMemberMetadata<
				FGBufferPassParameters, decltype(Completion),
				FGBufferPassResult>("Completion",
					offsetof(FGBufferPassParameters, Completion)),
			MakeRDGResourceParameterMemberMetadata<
				FGBufferPassParameters, decltype(Colors),
				FRDGColorAttachmentParameter>("Colors",
					offsetof(FGBufferPassParameters, Colors),
					ERDGParameterMemberKind::ManagedColorAttachment,
					ERDGResourceKind::Texture,
					ERDGParameterRangeKind::TextureSubresource,
					ERDGUse::ReadWrite,
					ERHIAccess::ColorAttachmentReadWrite, true,
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store, true,
					ERHIAccess::GraphicsShaderRead),
			MakeRDGResourceParameterMemberMetadata<
				FGBufferPassParameters, decltype(Depth),
				FRDGDepthStencilAttachmentParameter>("Depth",
					offsetof(FGBufferPassParameters, Depth),
					ERDGParameterMemberKind::ManagedDepthStencilAttachment,
					ERDGResourceKind::Texture,
					ERDGParameterRangeKind::TextureSubresource,
					ERDGUse::ReadWrite,
					ERHIAccess::DepthStencilReadWrite, true,
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store, true,
					ERHIAccess::GraphicsShaderRead),
		};
		static const auto Metadata =
			MakeInlineRDGParametersMetadata<FGBufferPassParameters>(
				"FGBufferPassParameters", Members);
		return &Metadata;
	}

	auto FGBufferGraphContributor::AddPasses(
		const FGBufferGraphInputs& Inputs) -> FGBufferGraphOutput
	{
		auto& Graph = Inputs.Graph;
		auto& Services = Inputs.Services;
		const auto RecordInputs = Inputs.Record;
		const auto& Options = Inputs.Options;
		const uint32 Width = Inputs.Width;
		const uint32 Height = Inputs.Height;
		const bool bNeedsGBuffer = Inputs.bNeedsGBuffer;
		const bool bWantsIsolatedDeferred = Inputs.bWantsIsolatedDeferred;
		std::array<std::optional<FRDGTextureHandle>, 4> GBuffer;
		const auto GBufferCompletion = Graph.CreateValue<FGBufferPassResult>(
			"Scene.GBufferValue", "gbuffer-result");
		if (Inputs.bEnabled)
		{
			const std::array Formats{EPixelFormat::RGBA8_UNORM,
				EPixelFormat::RGBA8_UNORM, EPixelFormat::RGBA8_UNORM,
				EPixelFormat::R11G11B10_FLOAT};
			const std::array Names{"Scene.GBuffer.Material", "Scene.GBuffer.Normals",
				"Scene.GBuffer.Surface", "Scene.GBuffer.Emissive"};
			for (uint32 Index = 0; Index < GBuffer.size(); ++Index)
				GBuffer[Index] = Graph.CreateTexture(
					FRDGTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
						Names[Index], Width, Height, Formats[Index])
						.SetFlags(ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource
							| ETextureCreateFlags::SourceCopy),
						.ObservationTag = static_cast<uint32>(
							ERDGAllocationObservation::GBuffer)}, Names[Index],
					ERHIAccess::GraphicsShaderRead);
		}
		auto Parameters = Graph.AllocParameters<FGBufferPassParameters>();
		Parameters->Completion = {GBufferCompletion};
		if (GBuffer[0])
		{
			for (uint32 Index = 0; Index < GBuffer.size(); ++Index)
			{
				Parameters->Colors[Index] = FRDGColorAttachmentParameter{
					.Texture = *GBuffer[Index],
					.Range = {ERHITextureAspect::Color, 0, 1, 0, 1}};
			}
			Parameters->Depth = FRDGDepthStencilAttachmentParameter{
				.Texture = Inputs.Depth,
				.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		}
		(void)AddSceneRenderFeaturePass<FGBufferGraphContributor>(
			Graph, ERDGPassType::Graphics, std::move(Parameters),
			[&Services, RecordInputs, &Options,
				Width, Height, bNeedsGBuffer, bWantsIsolatedDeferred](
				FRHICommandListImmediate& Commands,
				const FGBufferPassParameters& PassParameters,
				const FRDGParameterResolver& Resolver) {
				const FRDGAttachmentView Depth =
					Resolver.GetDepthStencilAttachment(PassParameters.Depth);
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = Depth.Texture};
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				const FRDGAttachmentView Material =
					Resolver.GetColorAttachment(PassParameters.Colors[0]);
				if (Material)
				{
					GBufferTargets = {
						.Material = Material.Texture,
						.Normals = Resolver.GetColorAttachment(
							PassParameters.Colors[1]).Texture,
						.Surface = Resolver.GetColorAttachment(
							PassParameters.Colors[2]).Texture,
						.Emissive = Resolver.GetColorAttachment(
							PassParameters.Colors[3]).Texture};
				}
				Resolver.WriteValue(PassParameters.Completion) =
					Services.Recorders.RenderGBuffer_RenderThread(
					Commands, RecordInputs, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					Options, Width, Height,
					bNeedsGBuffer, bWantsIsolatedDeferred);
			});
		return {.Completion = GBufferCompletion,
			.Textures = GBuffer, .Depth = Inputs.Depth};
	}

	auto FSceneRenderFeatureRecorders::RenderGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FGBufferRecordInputs& Inputs,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bNeedsGBuffer,
		bool bWantsIsolatedDeferred
	) -> FGBufferPassResult
	{
		const FSceneView& RenderView = Inputs.View;
		FGBufferPassResult Result;
		if (bNeedsGBuffer)
		{
			if (GBufferTargets == nullptr)
			{
				Result.Status = EScenePassStatus::Failed;
				++Telemetry.View.GBuffer.GBufferUnavailableViews;
				if (bWantsIsolatedDeferred)
					++Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
				if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled)
					++Telemetry.View.GBuffer.GBufferDebugFailures;
			}
			else
			{
				FRHIRenderPassInfo GBufferPassInfo{};
				GBufferPassInfo.RenderTargetLayout =
					RenderTargetLayouts::MakeGBufferTargets();
				GBufferPassInfo.ColorRenderTargets[0] =
					GBufferTargets->Material;
				GBufferPassInfo.ColorRenderTargets[1] =
					GBufferTargets->Normals;
				GBufferPassInfo.ColorRenderTargets[2] =
					GBufferTargets->Surface;
				GBufferPassInfo.ColorRenderTargets[3] =
					GBufferTargets->Emissive;
				GBufferPassInfo.DepthStencilRenderTarget = SceneTargets.Depth;
				for (uint32 Index = 0; Index < 4; ++Index)
				{
					GBufferPassInfo.ColorClearValues[Index] =
						FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f);
				}
				GBufferPassInfo.DepthStencilClearValue = FClearValueBinding(
					RenderView.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f,
					0u
				);
				const FGBufferTimingQuerySink GBufferTimingSink =
					GetGBufferTimingQuerySink();
				TScopedRendererGPUTimingQuery GBufferTiming(
					CommandList, GBufferTimingSink
				);
				CommandList.BeginRenderPass(
					GBufferPassInfo, "GBufferQualificationRenderPass"
				);
				CommandList.SetViewport(
					static_cast<float>(RenderView.ViewportX),
					static_cast<float>(RenderView.ViewportY),
					0.0f,
					static_cast<float>(RenderView.ViewportX + RenderView.ViewportWidth),
					static_cast<float>(RenderView.ViewportY + RenderView.ViewportHeight),
					1.0f
				);
				CommandList.SetScissor(
					static_cast<float>(RenderView.ViewportX),
					static_cast<float>(RenderView.ViewportY),
					static_cast<float>(RenderView.ViewportWidth),
					static_cast<float>(RenderView.ViewportHeight)
				);
				const FGeometryExecutionResult StaticResult = StaticMeshRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					Inputs.Receiver.StaticMeshes,
					ResolvedSceneResources.Receiver.StaticMeshes
				);
				const FGeometryExecutionResult SkeletalResult = SkeletalMeshRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					Inputs.Receiver.SkeletalMeshes,
					ResolvedSceneResources.Receiver.SkeletalMeshes
				);
				const FGeometryExecutionResult TerrainResult = TerrainRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					Inputs.Receiver.Terrains,
					ResolvedSceneResources.Receiver.Terrains
				);
				CommandList.EndRenderPass();
				Result.Status = StaticResult.bComplete && SkeletalResult.bComplete
					&& TerrainResult.bComplete
					? EScenePassStatus::Complete
					: EScenePassStatus::Failed;
				Result.bRenderedGeometry = StaticResult.bRenderedGeometry
					|| SkeletalResult.bRenderedGeometry
					|| TerrainResult.bRenderedGeometry;
				GBufferTiming.Commit();
				const FGBufferCaptureSink GBufferCaptureSink =
					GetGBufferCaptureSink();
				if (GBufferCaptureSink != nullptr)
				{
					GBufferCaptureSink(
						CommandList,
						GBufferTargets->Material,
						GBufferTargets->Normals,
						GBufferTargets->Surface,
						GBufferTargets->Emissive,
						SceneTargets.Depth
					);
				}
				++Telemetry.View.GBuffer.GBufferEnabledViews;
				Telemetry.View.GBuffer.GBufferAttachmentBytes =
					FGBufferRenderer::CalculateTargetBytes(Width, Height);
				Telemetry.View.GBuffer.GBufferAttemptedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferAttemptedDraws
					+ ResolvedSceneResources.Receiver.SkeletalMeshes.Observations.GBufferAttemptedDraws
					+ ResolvedSceneResources.Receiver.Terrains.Observations.GBufferAttemptedDraws;
				Telemetry.View.GBuffer.GBufferSuccessfulDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSuccessfulDraws
					+ ResolvedSceneResources.Receiver.SkeletalMeshes.Observations.GBufferSuccessfulDraws
					+ ResolvedSceneResources.Receiver.Terrains.Observations.GBufferSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferRejectedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferRejectedDraws
					+ ResolvedSceneResources.Receiver.SkeletalMeshes.Observations.GBufferRejectedDraws
					+ ResolvedSceneResources.Receiver.Terrains.Observations.GBufferRejectedDraws;
				Telemetry.View.GBuffer.GBufferSkippedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSkippedDraws
					+ ResolvedSceneResources.Receiver.SkeletalMeshes.Observations.GBufferSkippedDraws
					+ ResolvedSceneResources.Receiver.Terrains.Observations.GBufferSkippedDraws;
				Telemetry.View.GBuffer.GBufferStaticMeshAttemptedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferLocalAttemptedDraws;
				Telemetry.View.GBuffer.GBufferStaticMeshSuccessfulDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferLocalSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferStaticMeshRejectedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferLocalRejectedDraws;
				Telemetry.View.GBuffer.GBufferStaticMeshSkippedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferLocalSkippedDraws;
				Telemetry.View.GBuffer.GBufferSplineMeshAttemptedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSplineAttemptedDraws;
				Telemetry.View.GBuffer.GBufferSplineMeshSuccessfulDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSplineSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferSplineMeshRejectedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSplineRejectedDraws;
				Telemetry.View.GBuffer.GBufferSplineMeshSkippedDraws =
					ResolvedSceneResources.Receiver.StaticMeshes.Observations.GBufferSplineSkippedDraws;
				Telemetry.View.GBuffer.GBufferSkeletalMeshAttemptedDraws =
					ResolvedSceneResources.Receiver.SkeletalMeshes.Observations.GBufferAttemptedDraws;
				Telemetry.View.GBuffer.GBufferSkeletalMeshSuccessfulDraws =
					ResolvedSceneResources.Receiver.SkeletalMeshes.Observations.GBufferSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferSkeletalMeshRejectedDraws =
					ResolvedSceneResources.Receiver.SkeletalMeshes.Observations.GBufferRejectedDraws;
				Telemetry.View.GBuffer.GBufferSkeletalMeshSkippedDraws =
					ResolvedSceneResources.Receiver.SkeletalMeshes.Observations.GBufferSkippedDraws;
				Telemetry.View.GBuffer.GBufferTerrainAttemptedDraws =
					ResolvedSceneResources.Receiver.Terrains.Observations.GBufferAttemptedDraws;
				Telemetry.View.GBuffer.GBufferTerrainSuccessfulDraws =
					ResolvedSceneResources.Receiver.Terrains.Observations.GBufferSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferTerrainRejectedDraws =
					ResolvedSceneResources.Receiver.Terrains.Observations.GBufferRejectedDraws;
				Telemetry.View.GBuffer.GBufferTerrainSkippedDraws =
					ResolvedSceneResources.Receiver.Terrains.Observations.GBufferSkippedDraws;
			}
		}
		return Result;
	}
} // namespace Durin
