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
	auto FGBufferPassParameters::GetRenderGraphParametersMetadata()
		-> const FRenderGraphParametersMetadata*
	{
		static const std::array Members{
			MakeRenderGraphResourceParameterMemberMetadata<
				FGBufferPassParameters, decltype(Completion),
				FRenderGraphTokenParameter>("Completion",
					offsetof(FGBufferPassParameters, Completion),
					ERenderGraphParameterMemberKind::Token,
					ERenderGraphResourceKind::Token,
					ERenderGraphParameterRangeKind::None,
					ERenderGraphUse::Write, ERHIAccess::None, true),
			MakeRenderGraphResourceParameterMemberMetadata<
				FGBufferPassParameters, decltype(Colors),
				FRenderGraphColorAttachmentParameter>("Colors",
					offsetof(FGBufferPassParameters, Colors),
					ERenderGraphParameterMemberKind::ManagedColorAttachment,
					ERenderGraphResourceKind::Texture,
					ERenderGraphParameterRangeKind::TextureSubresource,
					ERenderGraphUse::ReadWrite,
					ERHIAccess::ColorAttachmentReadWrite, true,
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store, true,
					ERHIAccess::GraphicsShaderRead),
			MakeRenderGraphResourceParameterMemberMetadata<
				FGBufferPassParameters, decltype(Depth),
				FRenderGraphDepthStencilAttachmentParameter>("Depth",
					offsetof(FGBufferPassParameters, Depth),
					ERenderGraphParameterMemberKind::ManagedDepthStencilAttachment,
					ERenderGraphResourceKind::Texture,
					ERenderGraphParameterRangeKind::TextureSubresource,
					ERenderGraphUse::ReadWrite,
					ERHIAccess::DepthStencilReadWrite, true,
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store, true,
					ERHIAccess::GraphicsShaderRead),
		};
		static const auto Metadata =
			MakeInlineRenderGraphParametersMetadata<FGBufferPassParameters>(
				"FGBufferPassParameters", Members);
		return &Metadata;
	}

	auto FGBufferGraphContributor::AddPasses(
		FSceneFrameGraphContributorContext& Context,
		const FGBufferRecordInputs& RecordInputs) -> void
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
		if (Topology.bGBuffer)
		{
			const std::array Formats{EPixelFormat::RGBA8_UNORM,
				EPixelFormat::RGBA8_UNORM, EPixelFormat::RGBA8_UNORM,
				EPixelFormat::R11G11B10_FLOAT};
			const std::array Names{"Scene.GBuffer.Material", "Scene.GBuffer.Normals",
				"Scene.GBuffer.Surface", "Scene.GBuffer.Emissive"};
			for (uint32 Index = 0; Index < GraphResources.GBuffer.size(); ++Index)
				GraphResources.GBuffer[Index] = Graph.CreateTexture(Names[Index],
					FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
						Names[Index], Width, Height, Formats[Index])
						.SetFlags(ETextureCreateFlags::RenderTargetable
							| ETextureCreateFlags::ShaderResource
							| ETextureCreateFlags::SourceCopy),
						.BackingClass = std::string(GetSceneFrameBackingClassName(
							ESceneFrameBackingClass::GBuffer))},
					ERHIAccess::GraphicsShaderRead);
		}
		auto Parameters = Graph.AllocParameters<FGBufferPassParameters>();
		Parameters->Completion = {GBufferValue.Handle};
		if (GraphResources.GBuffer[0])
		{
			for (uint32 Index = 0; Index < GraphResources.GBuffer.size(); ++Index)
			{
				Parameters->Colors[Index] = FRenderGraphColorAttachmentParameter{
					.Texture = *GraphResources.GBuffer[Index],
					.Range = {ERHITextureAspect::Color, 0, 1, 0, 1}};
			}
			Parameters->Depth = FRenderGraphDepthStencilAttachmentParameter{
				.Texture = GraphResources.SceneDepth,
				.Range = {ERHITextureAspect::Depth, 0, 1, 0, 1}};
		}
		auto& GBufferResult = GBufferValue.Result;
		AddSceneFrameFeaturePass<FGBufferGraphContributor>(
			Graph, ERenderGraphPassType::Graphics, std::move(Parameters),
			[&Services, &GBufferResult, RecordInputs, &Options,
				Width, Height, bNeedsGBuffer, bWantsIsolatedDeferred](
				FRHICommandListImmediate& Commands,
				const FGBufferPassParameters& PassParameters,
				const FRenderGraphParameterResolver& Resolver) {
				const FRenderGraphAttachmentView Depth =
					Resolver.GetDepthStencilAttachment(PassParameters.Depth);
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = Depth.Texture};
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				const FRenderGraphAttachmentView Material =
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
				GBufferResult = Services.Recorders.RenderGBuffer_RenderThread(
					Commands, RecordInputs, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					Options, Width, Height,
					bNeedsGBuffer, bWantsIsolatedDeferred);
			});
	}

	auto FSceneFrameFeatureRecorders::RenderGBuffer_RenderThread(
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
					ResolvedFrame.Receiver.StaticMeshes
				);
				const FGeometryExecutionResult SkeletalResult = SkeletalMeshRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					Inputs.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes
				);
				const FGeometryExecutionResult TerrainResult = TerrainRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					Inputs.Receiver.Terrains,
					ResolvedFrame.Receiver.Terrains
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
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferAttemptedDraws
					+ ResolvedFrame.Receiver.SkeletalMeshes.Observations.GBufferAttemptedDraws
					+ ResolvedFrame.Receiver.Terrains.Observations.GBufferAttemptedDraws;
				Telemetry.View.GBuffer.GBufferSuccessfulDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferSuccessfulDraws
					+ ResolvedFrame.Receiver.SkeletalMeshes.Observations.GBufferSuccessfulDraws
					+ ResolvedFrame.Receiver.Terrains.Observations.GBufferSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferRejectedDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferRejectedDraws
					+ ResolvedFrame.Receiver.SkeletalMeshes.Observations.GBufferRejectedDraws
					+ ResolvedFrame.Receiver.Terrains.Observations.GBufferRejectedDraws;
				Telemetry.View.GBuffer.GBufferSkippedDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferSkippedDraws
					+ ResolvedFrame.Receiver.SkeletalMeshes.Observations.GBufferSkippedDraws
					+ ResolvedFrame.Receiver.Terrains.Observations.GBufferSkippedDraws;
				Telemetry.View.GBuffer.GBufferStaticMeshAttemptedDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferLocalAttemptedDraws;
				Telemetry.View.GBuffer.GBufferStaticMeshSuccessfulDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferLocalSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferStaticMeshRejectedDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferLocalRejectedDraws;
				Telemetry.View.GBuffer.GBufferStaticMeshSkippedDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferLocalSkippedDraws;
				Telemetry.View.GBuffer.GBufferSplineMeshAttemptedDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferSplineAttemptedDraws;
				Telemetry.View.GBuffer.GBufferSplineMeshSuccessfulDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferSplineSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferSplineMeshRejectedDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferSplineRejectedDraws;
				Telemetry.View.GBuffer.GBufferSplineMeshSkippedDraws =
					ResolvedFrame.Receiver.StaticMeshes.Observations.GBufferSplineSkippedDraws;
				Telemetry.View.GBuffer.GBufferSkeletalMeshAttemptedDraws =
					ResolvedFrame.Receiver.SkeletalMeshes.Observations.GBufferAttemptedDraws;
				Telemetry.View.GBuffer.GBufferSkeletalMeshSuccessfulDraws =
					ResolvedFrame.Receiver.SkeletalMeshes.Observations.GBufferSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferSkeletalMeshRejectedDraws =
					ResolvedFrame.Receiver.SkeletalMeshes.Observations.GBufferRejectedDraws;
				Telemetry.View.GBuffer.GBufferSkeletalMeshSkippedDraws =
					ResolvedFrame.Receiver.SkeletalMeshes.Observations.GBufferSkippedDraws;
				Telemetry.View.GBuffer.GBufferTerrainAttemptedDraws =
					ResolvedFrame.Receiver.Terrains.Observations.GBufferAttemptedDraws;
				Telemetry.View.GBuffer.GBufferTerrainSuccessfulDraws =
					ResolvedFrame.Receiver.Terrains.Observations.GBufferSuccessfulDraws;
				Telemetry.View.GBuffer.GBufferTerrainRejectedDraws =
					ResolvedFrame.Receiver.Terrains.Observations.GBufferRejectedDraws;
				Telemetry.View.GBuffer.GBufferTerrainSkippedDraws =
					ResolvedFrame.Receiver.Terrains.Observations.GBufferSkippedDraws;
			}
		}
		return Result;
	}
} // namespace Durin
