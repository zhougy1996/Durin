#include "Renderers/SceneFrameFeatureRecorders.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		auto CanonicalizeRenderGraphFrameCloudQuality(
			EVolumetricCloudQuality Quality) -> EVolumetricCloudQuality
		{
			return Quality < EVolumetricCloudQuality::Count
				? Quality : EVolumetricCloudQuality::High;
		}

		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}
	} // namespace

	FSceneFrameFeatureRecorders::FSceneFrameFeatureRecorders(
		FSceneRenderer& Renderer,
		FSceneRenderTelemetry& InTelemetry,
		FResolvedSceneFrame& InResolvedFrame,
		FSceneViewTemporalContext& InTemporalContext,
		FSceneViewState*& InViewState)
		: DefaultTextures(Renderer.DefaultTextures)
		, EnvironmentLighting(Renderer.EnvironmentLighting)
		, DirectionalShadowRenderer(Renderer.DirectionalShadowRenderer)
		, GBufferRenderer(Renderer.GBufferRenderer)
		, GBufferDebugRenderer(Renderer.GBufferDebugRenderer)
		, DeferredDirectionalLightingRenderer(
			Renderer.DeferredDirectionalLightingRenderer)
		, GroundTruthAmbientOcclusionRenderer(
			Renderer.GroundTruthAmbientOcclusionRenderer)
		, StaticMeshRenderer(Renderer.StaticMeshRenderer)
		, TerrainRenderer(Renderer.TerrainRenderer)
		, SkeletalMeshRenderer(Renderer.SkeletalMeshRenderer)
		, SkyBoxRenderer(Renderer.SkyBoxRenderer)
		, PostProcessRenderer(Renderer.PostProcessRenderer)
		, ContactShadowRenderer(Renderer.ContactShadowRenderer)
		, VolumetricCloudRenderer(Renderer.VolumetricCloudRenderer)
		, VolumetricCloudShadowRenderer(Renderer.VolumetricCloudShadowRenderer)
		, EditorAssistanceRenderer(Renderer.EditorAssistanceRenderer)
		, Qualification(GetRendererQualificationPolicy())
		, Telemetry(InTelemetry)
		, ResolvedFrame(InResolvedFrame)
		, TemporalContext(InTemporalContext)
		, ViewState(InViewState)
	{
	}

	auto FSceneFrameFeatureRecorders::RenderDirectionalShadow_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FDirectionalShadowRecordInputs& Inputs,
		FRHITexture* DirectionalShadowTarget
	) -> FDirectionalShadowPassResult
	{
		if (Inputs.Shadow == nullptr
			|| !ResolvedFrame.DirectionalShadow
			|| !ResolvedFrame.DirectionalShadow->bEnabled)
			return {};
		const bool bRendered = DirectionalShadowRenderer.Render_RenderThread(
			CommandList, DirectionalShadowTarget, StaticMeshRenderer, SkeletalMeshRenderer,
			TerrainRenderer, *Inputs.Shadow,
			*ResolvedFrame.DirectionalShadow, Telemetry.View);
		return {
			.Status = bRendered
				? EScenePassStatus::Complete : EScenePassStatus::Failed};
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

	auto FSceneFrameFeatureRecorders::RenderContactShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FContactVisibilityRecordInputs& Inputs,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FContactShadowVisibilityRenderer::FTargets*
			FragmentContactTargets,
		const FContactShadowVisibilityRenderer::FComputeTargets*
			ComputeContactTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete,
		bool bGBufferHasGeometry
	) -> FContactShadowPassResult
	{
		FContactShadowPassResult PassResult;
		const FSceneView& RenderView = Inputs.View;
		const bool bWantsContactVisibility = bWantsProductionDeferred
											 && RenderView.Settings.DirectionalShadow.bEnableContactShadows
											 && Inputs.Shadow != nullptr
											 && ResolvedFrame.DirectionalShadow
											 && ResolvedFrame.DirectionalShadow->bEnabled;
		if (!bWantsContactVisibility) return PassResult;
		PassResult.Status = EScenePassStatus::Failed;
		if (bWantsContactVisibility && bGBufferComplete
			&& bGBufferHasGeometry)
		{
			const EContactShadowRoutePreference RoutePreference =
				RenderView.Settings.DirectionalShadow.ContactRoutePreference;
			const bool bForceFragment = Qualification.bForceFragmentContactVisibility
										|| RoutePreference == EContactShadowRoutePreference::Fragment;
			const bool bForceCompute = !Qualification.bForceFragmentContactVisibility
									   && RoutePreference == EContactShadowRoutePreference::Compute;
			if (bForceCompute) FragmentContactTargets = nullptr;
			if (bForceFragment) ComputeContactTargets = nullptr;
			Telemetry.View.ContactShadow.ContactShadowRetainedBytes =
				ContactShadowRenderer.GetRetainedTargetBytes_RenderThread();
			const auto ContactResult = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, FragmentContactTargets, ComputeContactTargets,
				GBufferTargets->Material, GBufferTargets->Normals,
				GBufferTargets->Surface, GBufferTargets->Emissive,
				SceneTargets.Depth, RenderView,
				Inputs.Shadow->View.LightDirection, Width, Height,
				{.bGraphManagedTextureAccess = true}
			);
			const size_t ReasonIndex = static_cast<size_t>(ContactResult.Reason);
			if (ReasonIndex < Telemetry.View.ContactShadow.ContactShadowRouteReasons.size())
				++Telemetry.View.ContactShadow.ContactShadowRouteReasons[ReasonIndex];
			if (ContactResult.Visibility != nullptr)
			{
				Telemetry.View.ContactShadow.ContactShadowActiveBytes =
					FContactShadowVisibilityRenderer::CalculateTargetBytes(Width, Height);
				PassResult.Status = EScenePassStatus::Complete;
				PassResult.Route = ContactResult.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute
					? EContactShadowPassRoute::Compute
					: EContactShadowPassRoute::Fragment;
				PassResult.bDebug =
					RenderView.Settings.DirectionalShadow.bShowContactDebug;
				++Telemetry.View.ContactShadow.ContactShadowEnabledViews;
				if (ContactResult.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute)
				{
					++Telemetry.View.ContactShadow.ContactShadowComputeViews;
					++Telemetry.View.ContactShadow.ContactShadowDispatches;
				}
				else
				{
					++Telemetry.View.ContactShadow.ContactShadowFragmentViews;
					++Telemetry.View.ContactShadow.ContactShadowDraws;
				}
			}
			else
			{
				++Telemetry.View.ContactShadow.ContactShadowPassFailures;
				++Telemetry.View.ContactShadow.ContactShadowFactorOneViews;
			}
		}
		return PassResult;
	}

	auto FSceneFrameFeatureRecorders::RenderVolumetricCloudShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudShadowRecordInputs& Inputs,
		const FVolumetricCloudShadowRenderer::FTargets* FragmentTargets,
		const FVolumetricCloudShadowRenderer::FComputeTargets* ComputeTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		FRHITexture* BaseDensity,
		FRHITexture* DetailDensity,
		FRHITexture* Weather,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete
	) -> FVolumetricCloudShadowPassResult
	{
		FVolumetricCloudShadowPassResult PassResult;
		const FPreparedVolumetricCloud* Cloud = Inputs.Cloud;
		const FResolvedVolumetricCloud* ResolvedCloud =
			ResolvedFrame.VolumetricCloud
				? &*ResolvedFrame.VolumetricCloud : nullptr;
		const bool bRequested = bWantsProductionDeferred && bGBufferComplete
								&& Cloud != nullptr && ResolvedCloud != nullptr
								&& !Inputs.Lighting.Lights.Directional.empty()
								&& ResolvedCloud->Textures.BaseDensity
								&& ResolvedCloud->Textures.DetailDensity
								&& ResolvedCloud->Textures.DensitySampler
								&& SceneTargets.Depth;
		if (!bRequested) return PassResult;
		PassResult.Status = EScenePassStatus::Failed;
		const bool bForceFragment =
			Qualification.bForceFragmentVolumetricCloud;
		if (bForceFragment) ComputeTargets = nullptr;
		const auto QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
			Inputs.View.Settings.VolumetricCloud.Quality);
		const auto Result = VolumetricCloudShadowRenderer.Render_RenderThread(
			CommandList, FragmentTargets, ComputeTargets,
			{.bRequested = true,
				 .BaseDensity = BaseDensity,
				 .DetailDensity = DetailDensity,
			 .Weather = Weather,
			 .SceneDepth = SceneTargets.Depth,
				 .DensitySampler = ResolvedCloud->Textures.DensitySampler,
			 .Parameters = Cloud->Parameters,
				 .View = &Inputs.View,
			 .QualityTier = QualityTier,
			 .Width = Width,
			 .Height = Height},
			{.bGraphManagedTextureAccess = true}
		);
		auto& ViewTelemetry = Telemetry.View;
		const size_t ReasonIndex = static_cast<size_t>(Result.Reason);
		if (ReasonIndex < ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRouteReasons.size())
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRouteReasons[ReasonIndex];
		ViewTelemetry.VolumetricCloud.VolumetricCloudShadowRetainedBytes =
			VolumetricCloudShadowRenderer.GetRetainedTargetBytes_RenderThread();
		if (!Result.Visibility)
		{
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowFactorOneViews;
			return PassResult;
		}
		PassResult.Status = EScenePassStatus::Complete;
		PassResult.Route = Result.Route
			== FVolumetricCloudShadowRenderer::ERoute::Compute
			? EVolumetricCloudShadowPassRoute::Compute
			: EVolumetricCloudShadowPassRoute::Fragment;
		ViewTelemetry.VolumetricCloud.VolumetricCloudShadowActiveBytes = Result.TargetBytes;
		ViewTelemetry.VolumetricCloud.VolumetricCloudShadowSamples = static_cast<uint64>(Width)
												* Height * Result.SampleCount;
		++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowEnabledViews;
		if (Result.Route == FVolumetricCloudShadowRenderer::ERoute::Compute)
		{
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowComputeViews;
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowDispatches;
		}
		else
		{
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowFragmentViews;
			++ViewTelemetry.VolumetricCloud.VolumetricCloudShadowDraws;
		}
		return PassResult;
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

	auto FSceneFrameFeatureRecorders::RenderPostProcess_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FGBufferDebugRenderer::FTargets* GBufferDebugTargets,
		FRHITexture* SceneColor,
		FRHITexture* GroundTruthAmbientOcclusionDebugOutput,
		bool bEditorAssistanceFollows
	) -> FPostProcessPassResult
	{
		const uint32 Width = OutputTarget->GetSizeX();
		const uint32 Height = OutputTarget->GetSizeY();
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		FRHITexture* PostProcessInput = SceneColor;
		if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled
			&& GBufferTargets != nullptr)
		{
			if (GBufferDebugTargets != nullptr
				&& GBufferDebugRenderer.Render_RenderThread(
					CommandList,
					GBufferTargets->Material,
					GBufferTargets->Normals,
					GBufferTargets->Surface,
					GBufferTargets->Emissive,
					SceneTargets.Depth,
					GBufferDebugTargets->Color,
					RenderView,
					Options.GBufferDebugMode,
					Width,
					Height
				))
			{
				PostProcessInput = GBufferDebugTargets->Color;
				++Telemetry.View.GBuffer.GBufferDebugViews;
			}
			else
			{
				++Telemetry.View.GBuffer.GBufferDebugFailures;
			}
		}
		else if (GroundTruthAmbientOcclusionDebugOutput != nullptr)
		{
			PostProcessInput = GroundTruthAmbientOcclusionDebugOutput;
		}
		const FHDRSceneColorCaptureSink HDRCaptureSink =
			GetHDRSceneColorCaptureSink();
		if (HDRCaptureSink != nullptr)
		{
			HDRCaptureSink(CommandList, SceneColor, PostProcessInput);
		}

		FRHIRenderPassInfo PostProcessPassInfo{};
		PostProcessPassInfo.RenderTargetLayout = bEditorAssistanceFollows
			? RenderTargetLayouts::MakeScenePostProcessOutput()
			: RenderTargetLayouts::MakeFinalScenePostProcessOutput(ViewportOutput);
		PostProcessPassInfo.ColorRenderTargets[0] = OutputTarget;
		PostProcessPassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r,
			View.ClearColor.g,
			View.ClearColor.b,
			View.ClearColor.a
		);
		const FPostProcessTimingQuerySink PostProcessTimingSink =
			GetPostProcessTimingQuerySink();
		TScopedRendererGPUTimingQuery PostProcessTiming(
			CommandList, PostProcessTimingSink
		);
		CommandList.BeginRenderPass(
			PostProcessPassInfo,
			bPresentOutput ? "PostProcessPresentRenderPass" : "PostProcessOffscreenRenderPass"
		);
		PostProcessRenderer.Draw_RenderThread(
			CommandList,
			PostProcessInput,
			Width,
			Height,
			bPresentOutput,
			View.Settings.PostProcess.bEnableFXAA,
			bEditorAssistanceFollows,
			RenderView.Settings.PostProcess.ExposureEV
		);
		CommandList.EndRenderPass();
		PostProcessTiming.Commit();
		return {.Result = ERenderViewResult::Success};
	}

	auto FSceneFrameFeatureRecorders::RenderEditorAssistance_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& RenderView,
		FRHITexture* OutputTarget,
		FRHITexture* DepthTarget,
		bool bPresentOutput,
		const RendererEditorAssistance::FPrepared& Prepared
	) -> bool
	{
		if (!Prepared.HasDrawableOperation()) return false;
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		FRHIRenderPassInfo EditorAssistancePassInfo{};
		EditorAssistancePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeEditorAssistanceOutput(ViewportOutput);
		EditorAssistancePassInfo.ColorRenderTargets[0] = OutputTarget;
		EditorAssistancePassInfo.DepthStencilRenderTarget =
			DepthTarget;
		CommandList.BeginRenderPass(
			EditorAssistancePassInfo,
			bPresentOutput ? "EditorAssistancePresentRenderPass" : "EditorAssistanceOffscreenRenderPass"
		);
		EditorAssistanceRenderer.Draw_RenderThread(
			CommandList,
			RenderView,
			Prepared
		);
		CommandList.EndRenderPass();
		return true;
	}

	auto FSceneFrameFeatureRecorders::RenderVolumetricCloudSpatial_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudRecordInputs& Inputs,
		const FVolumetricCloudRenderer::FTargets* FragmentTargets,
		const FVolumetricCloudRenderer::FComputeTargets* ComputeTargets,
		FRHITexture* BaseDensity,
		FRHITexture* DetailDensity,
		FRHITexture* Weather,
		FRHITexture* Depth
	) -> FVolumetricCloudSpatialPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = Inputs.View;
		const uint32 Width = Depth != nullptr ? Depth->GetSizeX() : 0;
		const uint32 Height = Depth != nullptr ? Depth->GetSizeY() : 0;
		const FPreparedVolumetricCloud* Cloud = Inputs.Cloud;
		const FResolvedVolumetricCloud* ResolvedCloud =
			ResolvedFrame.VolumetricCloud
				? &*ResolvedFrame.VolumetricCloud : nullptr;
		const bool bInputsPresent = Cloud != nullptr && ResolvedCloud != nullptr
									&& BaseDensity != nullptr
									&& DetailDensity != nullptr
									&& Weather != nullptr
									&& ResolvedCloud->Textures.DensitySampler != nullptr
									&& Depth != nullptr;
		const auto QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
			View.Settings.VolumetricCloud.Quality);
		const auto Quality = FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(
			QualityTier
		);
		const auto CloudExtent = FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
			Width, Height, Quality
		);
		if (!bInputsPresent) FragmentTargets = nullptr;
		if (!bInputsPresent || Qualification.bForceFragmentVolumetricCloud)
			ComputeTargets = nullptr;
		auto Textures = ResolvedCloud != nullptr
			? ResolvedCloud->Textures
			: FVolumetricCloudRenderer::FTextureBindings{};
		Textures.BaseDensity = BaseDensity;
		Textures.DetailDensity = DetailDensity;
		Textures.Weather = Weather;
		Textures.SceneDepth = Depth;
		const FVolumetricCloudRenderer::FRenderResult Result =
			VolumetricCloudRenderer.Render_RenderThread(CommandList, FragmentTargets,
				ComputeTargets, {.bRequested = Cloud != nullptr, .Textures = Textures,
					.Parameters = Cloud != nullptr ? Cloud->Parameters
						: FVolumetricCloudRenderer::FParameters{}, .View = &View,
					.QualityTier = QualityTier,
					.SuccessfulSequence = TemporalContext.SuccessfulSequence,
					.Width = CloudExtent.Width, .Height = CloudExtent.Height,
					.OutputWidth = Width, .OutputHeight = Height},
				{.bGraphManagedTextureAccess = true});
		auto& ViewTelemetry = Telemetry.View;
		const auto RouteIndex = static_cast<size_t>(Result.Counters.Reason);
		if (RouteIndex < ViewTelemetry.VolumetricCloud.VolumetricCloudRouteReasons.size())
			++ViewTelemetry.VolumetricCloud.VolumetricCloudRouteReasons[RouteIndex];
		ViewTelemetry.VolumetricCloud.VolumetricCloudDispatches += Result.Counters.Dispatches;
		ViewTelemetry.VolumetricCloud.VolumetricCloudDraws += Result.Counters.Draws;
		ViewTelemetry.VolumetricCloud.VolumetricCloudPrimarySamples += Result.Counters.PrimarySamples;
		ViewTelemetry.VolumetricCloud.VolumetricCloudLightSamples += Result.Counters.LightSamples;
		ViewTelemetry.VolumetricCloud.VolumetricCloudTargetWidth = Result.Counters.TargetWidth;
		ViewTelemetry.VolumetricCloud.VolumetricCloudTargetHeight = Result.Counters.TargetHeight;
		ViewTelemetry.VolumetricCloud.VolumetricCloudOutputWidth = Result.Counters.OutputWidth;
		ViewTelemetry.VolumetricCloud.VolumetricCloudOutputHeight = Result.Counters.OutputHeight;
		ViewTelemetry.VolumetricCloud.VolumetricCloudActiveBytes = Result.Counters.TargetBytes;
		if (Result.Counters.Route == FVolumetricCloudRenderer::ERoute::Compute)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudComputeViews;
		else if (Result.Counters.Route == FVolumetricCloudRenderer::ERoute::Fragment)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudFragmentViews;
		else
			++ViewTelemetry.VolumetricCloud.VolumetricCloudDisabledViews;
		return {
			.Status = Result.Cloud != nullptr
				? EScenePassStatus::Complete
				: (Cloud != nullptr ? EScenePassStatus::Failed
					: EScenePassStatus::NotRequested),
			.Route = Result.Counters.Route};
	}

	auto FSceneFrameFeatureRecorders::RenderVolumetricCloudComposite_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FVolumetricCloudRecordInputs& Inputs,
		const FVolumetricCloudSpatialPassResult& Spatial,
		const FVolumetricCloudRenderer::FTargets* FragmentTargets,
		const FVolumetricCloudRenderer::FComputeTargets* ComputeTargets,
		const FVolumetricCloudRenderer::FTargets* CompositeTargets,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		FRHITexture* VolumetricCloudShadowVisibility
	) -> FVolumetricCloudPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = Inputs.View;
		const FPreparedVolumetricCloud* Cloud = Inputs.Cloud;
		const auto QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
			View.Settings.VolumetricCloud.Quality);
		auto& ViewTelemetry = Telemetry.View;
		FRHITexture* CurrentCloud = Spatial.Route
				== FVolumetricCloudRenderer::ERoute::Compute && ComputeTargets
			? ComputeTargets->Cloud.GetReference()
			: (Spatial.Route == FVolumetricCloudRenderer::ERoute::Fragment
				&& FragmentTargets ? FragmentTargets->Cloud.GetReference() : nullptr);
		const FVolumetricCloudRenderer::FTemporalReconstructionResult Temporal =
			CurrentCloud != nullptr ? VolumetricCloudRenderer.ReconstructTemporal_RenderThread(
										  CommandList, {.CurrentCloud = CurrentCloud, .View = &View, .TemporalContext = &TemporalContext, .ViewState = ViewState, .Parameters = Cloud != nullptr ? Cloud->Parameters : FVolumetricCloudRenderer::FParameters{}, .QualityTier = QualityTier, .CloudHistoryKey = Cloud != nullptr ? Cloud->HistoryKey : 0}
									  ) :
									  FVolumetricCloudRenderer::FTemporalReconstructionResult{};
		ViewTelemetry.VolumetricCloud.VolumetricCloudHistoryBytes = Temporal.HistoryBytes;
		if (Temporal.bCandidatePublished)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudTemporalDraws;
		if (Temporal.bHistoryAccepted)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudHistoryAccepted;
		else if (Temporal.bCandidatePublished)
			++ViewTelemetry.VolumetricCloud.VolumetricCloudHistoryRejected;
		FRHITexture* Composite = Temporal.Cloud != nullptr
			&& CompositeTargets != nullptr
			? VolumetricCloudRenderer.Composite_RenderThread(
				CommandList,
				*CompositeTargets,
				SceneColor, Temporal.Cloud, Depth,
				VolumetricCloudShadowVisibility,
				Temporal.bCandidatePublished,
				Temporal.bHistoryAccepted, View) :
			nullptr;
		ViewTelemetry.VolumetricCloud.VolumetricCloudRetainedBytes =
			VolumetricCloudRenderer.GetRetainedTargetBytes_RenderThread();
		if (Composite != nullptr)
		{
			++ViewTelemetry.VolumetricCloud.VolumetricCloudEnabledViews;
			++ViewTelemetry.VolumetricCloud.VolumetricCloudCompositeDraws;
			return {
				.Status = EScenePassStatus::Complete,
				.bCompositeOutputValid = true};
		}
		return {
			.Status = Spatial.Status == EScenePassStatus::Complete
				? EScenePassStatus::Failed : EScenePassStatus::NotRequested};
	}

	auto FSceneFrameFeatureRecorders::RenderSceneOpaque_RenderThread(
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
			const bool bRendered = RenderSpecialForwardScene_RenderThread(
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
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						Inputs.Receiver.StaticMeshes,
						ResolvedFrame.Receiver.StaticMeshes, true
					);
				}
			const auto& SkeletalDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.SkeletalMeshes.Opaque : Inputs.Receiver.SkeletalMeshes.Masked;
			for (const FPreparedSkeletalMeshDraw& Draw : SkeletalDraws)
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						Inputs.Receiver.SkeletalMeshes,
						ResolvedFrame.Receiver.SkeletalMeshes, true
					);
				}
			const auto& TerrainDraws = Pass == EMeshBasePass::Opaque ? Inputs.Receiver.Terrains.Opaque : Inputs.Receiver.Terrains.Masked;
			for (const FPreparedTerrainDraw& Draw : TerrainDraws)
				if (Draw.Material.PlanningPassIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					TerrainRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Draw,
						Inputs.Receiver.Terrains,
						ResolvedFrame.Receiver.Terrains,
						true
					);
				}
		}
		CommandList.EndRenderPass();
		RetainedOpaqueTiming.Commit();
		return {.Result = ERenderViewResult::Success};
	}

	auto FSceneFrameFeatureRecorders::RenderSceneTranslucency_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneGeometryRecordInputs& Inputs,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		const FSceneColorPassResult& Opaque,
		const FVolumetricCloudPassResult& VolumetricCloud
	) -> FSceneColorPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (!Opaque.IsSuccess()) return Opaque;
		const FSceneView& View = Inputs.View;
		if (View.Settings.Mode.RenderMode != ERenderMode::Lit
			|| View.Settings.Mode.RasterMode != ERasterMode::Solid)
			return Opaque;
		if (SceneColor == nullptr || Depth == nullptr) return {};
		auto SetViewRect = [&CommandList, &View]() {
			CommandList.SetViewport(
				static_cast<float>(View.ViewportX),
				static_cast<float>(View.ViewportY), 0.0f,
				static_cast<float>(View.ViewportX + View.ViewportWidth),
				static_cast<float>(View.ViewportY + View.ViewportHeight), 1.0f);
			CommandList.SetScissor(
				static_cast<float>(View.ViewportX),
				static_cast<float>(View.ViewportY),
				static_cast<float>(View.ViewportWidth),
				static_cast<float>(View.ViewportHeight));
		};
		const FSortedTranslucencyTimingQuerySink SortedTranslucencyTimingSink =
			GetSortedTranslucencyTimingQuerySink();
		TScopedRendererGPUTimingQuery SortedTranslucencyTiming(
			CommandList, SortedTranslucencyTimingSink
		);

		FRHIRenderPassInfo SortedTranslucency{};
		SortedTranslucency.RenderTargetLayout =
			RenderTargetLayouts::MakeHybridSortedTranslucency();
		SortedTranslucency.ColorRenderTargets[0] = SceneColor;
		SortedTranslucency.DepthStencilRenderTarget = Depth;
		CommandList.BeginRenderPass(
			SortedTranslucency, "HybridSortedTranslucencyRenderPass"
		);
		SetViewRect();
		for (const FPreparedTranslucentSceneDraw& Draw :
			 Inputs.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.StaticMeshes,
					ResolvedFrame.Receiver.StaticMeshes, true
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes, true
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					Inputs.Receiver.Terrains.Translucent[Draw.DrawIndex],
					Inputs.Receiver.Terrains,
					ResolvedFrame.Receiver.Terrains, true
				);
		}
		CommandList.EndRenderPass();
		SortedTranslucencyTiming.Commit();
		// Lit opaque/masked sections were already consumed by GBuffer + deferred
		// lighting, so the retained-forward attempted count intentionally does not
		// equal every prepared section as it does in the all-forward finalizer.
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.StaticMeshes);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.SkeletalMeshes);
		TerrainRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.Terrains);
		++Telemetry.View.Deferred.HybridDeferredEnabledViews;
		return {
			.Result = ERenderViewResult::Success,
			.bUsesVolumetricCloudComposite =
				VolumetricCloud.bCompositeOutputValid,
			.VolumetricCloud = VolumetricCloud};
	}

	auto FSceneFrameFeatureRecorders::RenderSpecialForwardScene_RenderThread(
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
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.StaticMeshes,
				ResolvedFrame.Receiver.StaticMeshes
			);
			SkeletalMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.SkeletalMeshes,
				ResolvedFrame.Receiver.SkeletalMeshes
			);
			TerrainRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				Inputs.Receiver.Terrains,
				ResolvedFrame.Receiver.Terrains
			);
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 Inputs.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.StaticMeshes,
					ResolvedFrame.Receiver.StaticMeshes
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					Inputs.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					Inputs.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					Inputs.Receiver.Terrains.Translucent[Draw.DrawIndex],
					Inputs.Receiver.Terrains,
					ResolvedFrame.Receiver.Terrains
				);
		}
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.StaticMeshes
		);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.SkeletalMeshes
		);
		TerrainRenderer.FinalizeExecution_RenderThread(
			ResolvedFrame.Receiver.Terrains);
		return true;
	}
} // namespace Durin
