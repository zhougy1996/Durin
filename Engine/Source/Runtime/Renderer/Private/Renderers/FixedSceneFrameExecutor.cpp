#include "Renderers/FixedSceneFrameExecutor.h"

#include "Renderers/SceneRenderer.h"
#include "Renderers/SceneFrameFinalization.h"
#include "Renderers/SceneFramePreparation.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "Scene.h"
#include "SceneView.h"

#include <limits>

namespace Durin
{
	namespace
	{
		auto ReportFixedFrameRejectedViewState(
			std::string_view Reason,
			FSceneViewStateId Id
		) -> void
		{
			static uint32 DiagnosticCount = 0;
			if (DiagnosticCount >= 16) return;
			++DiagnosticCount;
			DURIN_WARN(
				"Renderer rejected {} view-state identity {}.",
				Reason, FSceneViewStateIdAccess::GetValue(Id)
			);
		}

		class FFixedFrameViewStateSubmission final
		{
		public:
			explicit FFixedFrameViewStateSubmission(FSceneViewState* InState)
				: State(InState)
			{
			}

			~FFixedFrameViewStateSubmission()
			{
				if (State != nullptr) State->Abort();
			}

			auto Commit() -> void
			{
				if (State != nullptr)
				{
					State->Commit();
					State = nullptr;
				}
			}

		private:
			FSceneViewState* State = nullptr;
		};

		auto CanonicalizeFixedFrameCloudQuality(EVolumetricCloudQuality Quality)
			-> EVolumetricCloudQuality
		{
			return Quality < EVolumetricCloudQuality::Count
				? Quality : EVolumetricCloudQuality::High;
		}

		auto CanonicalizeFixedFrameCloudDebugMode(EVolumetricCloudDebugMode Mode)
			-> EVolumetricCloudDebugMode
		{
			return Mode < EVolumetricCloudDebugMode::Count
				? Mode : EVolumetricCloudDebugMode::Lit;
		}

		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}
	} // namespace

	auto FFixedSceneFrameExecutor::Execute_RenderThread(
		FSceneRenderer& Renderer,
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics
	) const -> ERenderViewResult
	{
		return Renderer.ExecuteFixedFrame_RenderThread(
			CommandList, Scene, View, OutputTarget, bPresentOutput, Options,
			OutStatistics
		);
	}

	auto FSceneRenderer::ExecuteFixedFrame_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics
	) -> ERenderViewResult
	{
		check(IsInRenderingThread());
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderView");
		if (RenderSubmissionSerial != std::numeric_limits<uint64>::max())
			++RenderSubmissionSerial;
		FSceneRenderPlan PreparedView;
		PreparedView.Telemetry.Counters.VolumetricCloudQuality =
			CanonicalizeFixedFrameCloudQuality(View.Settings.VolumetricCloud.Quality);
		PreparedView.Telemetry.Counters.VolumetricCloudDebugMode =
			CanonicalizeFixedFrameCloudDebugMode(View.Settings.VolumetricCloud.DebugMode);
		FSceneTelemetryPublication TelemetryPublication(
			PreparedView.Telemetry, OutStatistics
		);
		const uint32 Width =
			OutputTarget != nullptr ? OutputTarget->GetSizeX() : 0;
		const uint32 Height =
			OutputTarget != nullptr ? OutputTarget->GetSizeY() : 0;
		if (OutputTarget == nullptr || Width == 0 || Height == 0)
		{
			return ERenderViewResult::InvalidOutput;
		}
		if (!PostProcessRenderer.EnsureResources_RenderThread(CommandList))
		{
			return ERenderViewResult::RendererResourcesUnavailable;
		}
		// Generated IBL uploads must finish before entering the Scene Color pass.
		// Failure is non-fatal: StaticMeshRenderer binds the complete black
		// environment fallback set instead.
		EnvironmentLighting.EnsureResources_RenderThread(CommandList);
		// Sky resources include a static index upload, so initialize them before
		// entering the Scene Color render pass.
		const bool bSkyBoxResourcesReady =
			SkyBoxRenderer.EnsureResources_RenderThread();
		if (Options.Environment && !bSkyBoxResourcesReady)
		{
			return ERenderViewResult::RendererResourcesUnavailable;
		}
		FPostProcessRenderer::FSceneTargets* SceneTargets =
			PostProcessRenderer.EnsureSceneTargets_RenderThread(Width, Height);
		if (SceneTargets == nullptr || SceneTargets->Color == nullptr
			|| SceneTargets->Depth == nullptr)
		{
			return ERenderViewResult::RendererResourcesUnavailable;
		}
		FRHITexture* SceneColor = SceneTargets->Color;

		FSceneView RenderView = FitViewToOutput(View, Width, Height);
		FSceneViewState* ViewState = ViewStates.Find(RenderView.ViewStateId);
		if (ViewState != nullptr && ViewState->IsSubmissionActive())
		{
			PreparedView.Context.TemporalContext.Current =
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				);
			PreparedView.Context.TemporalContext.SubmissionSerial =
				RenderSubmissionSerial;
			PreparedView.Context.TemporalContext.Discontinuities =
				ESceneViewDiscontinuity::DuplicateSubmission;
			ReportFixedFrameRejectedViewState(
				"an interleaved submission for",
				RenderView.ViewStateId
			);
			ViewState = nullptr;
		}
		FFixedFrameViewStateSubmission ViewStateSubmission(ViewState);
		if (ViewState != nullptr)
		{
			PreparedView.Context.TemporalContext = ViewState->Begin(
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				),
				RenderSubmissionSerial, RenderView.bDiscardHistory
			);
		}
		else if (PreparedView.Context.TemporalContext.Discontinuities
				 != ESceneViewDiscontinuity::DuplicateSubmission)
		{
			PreparedView.Context.TemporalContext.Current =
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				);
			PreparedView.Context.TemporalContext.SubmissionSerial =
				RenderSubmissionSerial;
			PreparedView.Context.TemporalContext.Discontinuities =
				ESceneViewDiscontinuity::MissingState;
			if (RenderView.ViewStateId.IsValid())
			{
				ReportFixedFrameRejectedViewState(
					"a missing, released, or foreign",
					RenderView.ViewStateId
				);
			}
		}
		PreparedView.Context.ViewState = ViewState;
		const ERenderViewResult PreparationResult = FSceneFramePreparation{}.Prepare_RenderThread(*this,
			CommandList, Scene, RenderView, Options, PreparedView
		);
		if (PreparationResult != ERenderViewResult::Success)
			return PreparationResult;
		FRHITexture* DirectionalShadowTexture =
			DirectionalShadowRenderer.GetTexture_RenderThread();
		FRHISampler* DirectionalShadowSampler =
			DirectionalShadowRenderer.GetSampler_RenderThread();

		FGBufferRenderer::FTargets* GBufferTargets = nullptr;
		const bool bRequiresDeferredOpaque =
			RenderView.Settings.Mode.RenderMode == ERenderMode::Lit
			&& RenderView.Settings.Mode.RasterMode == ERasterMode::Solid;
		const bool bWantsIsolatedDeferred =
			Options.bEnableDeferredDirectionalQualification
			|| Options.DeferredDirectionalDebugMode
				   != EDeferredDirectionalDebugMode::Disabled
			|| Options.GroundTruthAmbientOcclusionDebugMode
				   != EGroundTruthAmbientOcclusionDebugMode::Disabled;
		const bool bWantsIsolatedGroundTruthAmbientOcclusion =
			Options.bEnableGroundTruthAmbientOcclusionQualification
			|| Options.GroundTruthAmbientOcclusionDebugMode
				   != EGroundTruthAmbientOcclusionDebugMode::Disabled;
		const bool bWantsProductionDeferred = bRequiresDeferredOpaque;
		const bool bWantsProductionGroundTruthAmbientOcclusion =
			bWantsProductionDeferred
			&& RenderView.Settings.AmbientOcclusion.bEnabled;
		const bool bWantsGroundTruthAmbientOcclusion =
			bWantsIsolatedGroundTruthAmbientOcclusion
			|| bWantsProductionGroundTruthAmbientOcclusion;
		const bool bWantsDeferredInputs = bWantsIsolatedDeferred
										  || bWantsProductionDeferred
										  || bWantsGroundTruthAmbientOcclusion;
		const bool bHybridRetainedResourcesReady =
			!bWantsProductionDeferred
			|| (StaticMeshRenderer.PrepareHybridRetainedResources_RenderThread(
					PreparedView.Receiver.StaticMeshes,
					PreparedView.ResolvedReceiver.StaticMeshes
				)
				&& SkeletalMeshRenderer.PrepareHybridRetainedResources_RenderThread(
					PreparedView.Receiver.SkeletalMeshes,
					PreparedView.ResolvedReceiver.SkeletalMeshes
				)
				&& TerrainRenderer.PrepareHybridRetainedResources_RenderThread(
					CommandList, PreparedView.Receiver.Terrains,
					PreparedView.ResolvedReceiver.Terrains
				));
		const bool bNeedsGBuffer = Options.bEnableGBufferQualification
								   || Options.GBufferDebugMode != EGBufferDebugMode::Disabled
								   || bWantsDeferredInputs;
		const FGBufferPassResult GBufferResult = RenderGBuffer_RenderThread(
			CommandList, PreparedView, SceneTargets, Options, Width, Height,
			bNeedsGBuffer, bWantsIsolatedDeferred
		);
		GBufferTargets = GBufferResult.Targets;

		const bool bGBufferComplete = GBufferResult.IsComplete();
		FDeferredDirectionalLightingRenderer::FRenderParameters DeferredParameters;
		FRHITexture* GroundTruthAmbientOcclusionFallback =
			DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		FRHITexture* ContactVisibilityFallback =
			DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		FRHITexture* GroundTruthAmbientOcclusionDebugOutput = nullptr;
		if (bWantsDeferredInputs && GBufferTargets != nullptr)
		{
			if (bGBufferComplete)
			{
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
						EDefaultTexture::Black
					);
					EnvironmentSampler = nullptr;
				}
				DeferredParameters = {
					.Material = GBufferTargets->Material,
					.Normals = GBufferTargets->Normals,
					.Surface = GBufferTargets->Surface,
					.Emissive = GBufferTargets->Emissive,
					.Depth = SceneTargets->Depth,
					.EnvironmentIrradiance = EnvironmentIrradiance,
					.EnvironmentPrefiltered = EnvironmentPrefiltered,
					.EnvironmentBrdfLut = EnvironmentBrdfLut,
					.EnvironmentSampler = EnvironmentSampler,
					.DirectionalShadowTexture = DirectionalShadowTexture != nullptr ? DirectionalShadowTexture : DefaultTextures.GetArray_RenderThread(),
					.DirectionalShadowSampler = DirectionalShadowSampler,
					.GroundTruthAmbientOcclusionRaw =
						GroundTruthAmbientOcclusionFallback,
					.GroundTruthAmbientOcclusionFiltered =
						GroundTruthAmbientOcclusionFallback,
					.GroundTruthAmbientOcclusionResolved =
						GroundTruthAmbientOcclusionFallback,
					.GroundTruthAmbientOcclusionSelector =
						GroundTruthAmbientOcclusionFallback,
					.ContactVisibility = ContactVisibilityFallback,
					.VolumetricCloudVisibility = DefaultTextures.Get_RenderThread(
						EDefaultTexture::White
					),
					.Lighting = PreparedView.Lighting.UniformBuffer,
					.View = &RenderView,
					.DiagnosticMode = static_cast<uint32>(
						Options.DeferredDirectionalDebugMode
					)
				};
			}
		}

		RenderGroundTruthAmbientOcclusion_RenderThread(
			CommandList, PreparedView, GBufferTargets, SceneTargets,
			DeferredParameters, Options, Width, Height,
			bWantsGroundTruthAmbientOcclusion, bGBufferComplete,
			GroundTruthAmbientOcclusionFallback
		);

		RenderContactShadows_RenderThread(
			CommandList, PreparedView, GBufferTargets, SceneTargets,
			DeferredParameters, Options, Width, Height,
			bWantsProductionDeferred, bGBufferComplete,
			GBufferResult.bRenderedGeometry
		);
		RenderVolumetricCloudShadows_RenderThread(
			CommandList, PreparedView, SceneTargets, DeferredParameters,
			Width, Height, bWantsProductionDeferred, bGBufferComplete
		);

		GroundTruthAmbientOcclusionDebugOutput =
			RenderIsolatedDeferred_RenderThread(
				CommandList, PreparedView, DeferredParameters, Options,
				Width, Height, bWantsIsolatedDeferred, bGBufferComplete
			);

		const bool bProductionResourcesReady =
			!bWantsProductionDeferred
			|| (bGBufferComplete && bHybridRetainedResourcesReady);
		if (bWantsProductionDeferred)
			DeferredParameters.DiagnosticMode = 0;
		const FSceneColorTimingQuerySink SceneColorTimingSink =
			GetSceneColorTimingQuerySink();
		TScopedRendererGPUTimingQuery SceneColorTiming(
			CommandList, SceneColorTimingSink
		);
		const ERenderViewResult SceneResult = RenderScene_RenderThread(
			CommandList, PreparedView, SceneColor, SceneTargets->Depth,
			bWantsProductionDeferred && bProductionResourcesReady ? &DeferredParameters : nullptr
		);
		SceneColorTiming.Commit();
		if (SceneResult != ERenderViewResult::Success)
			return SceneResult;
		ReduceStaticMeshTelemetry(
			PreparedView.Receiver.StaticMeshes,
			PreparedView.ResolvedReceiver.StaticMeshes, PreparedView.Telemetry.Counters
		);
		ReduceSkeletalMeshTelemetry(
			PreparedView.Receiver.SkeletalMeshes,
			PreparedView.ResolvedReceiver.SkeletalMeshes,
			PreparedView.Receiver.SkeletalPalettes,
			PreparedView.Telemetry.Counters
		);
		ReduceTerrainTelemetry(PreparedView.Receiver.Terrains,
			PreparedView.ResolvedReceiver.Terrains, PreparedView.Telemetry.Counters);

		const ERenderViewResult Result = FSceneFrameFinalization{}.Finalize_RenderThread(
			*this, CommandList, PreparedView, View, OutputTarget, bPresentOutput,
			Options, SceneTargets, GBufferTargets, SceneColor,
			GroundTruthAmbientOcclusionDebugOutput
		);
		if (Result == ERenderViewResult::Success)
		{
			ViewStateSubmission.Commit();
			TelemetryPublication.Commit();
		}
		return Result;
	}


	auto FSceneRenderer::RenderGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneRenderPlan& PreparedView,
		FPostProcessRenderer::FSceneTargets* SceneTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bNeedsGBuffer,
		bool bWantsIsolatedDeferred
	) -> FGBufferPassResult
	{
		const FSceneView& RenderView = PreparedView.Context.View;
		FGBufferPassResult Result;
		FGBufferRenderer::FTargets* GBufferTargets = nullptr;
		if (bNeedsGBuffer)
		{
			GBufferTargets =
				GBufferRenderer.EnsureTargets_RenderThread(Width, Height);
			if (GBufferTargets == nullptr)
			{
				Result.Status = EScenePassStatus::Failed;
				++PreparedView.Telemetry.Counters.GBufferUnavailableViews;
				if (bWantsIsolatedDeferred)
					++PreparedView.Telemetry.Counters.DeferredDirectionalUnavailableViews;
				if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled)
					++PreparedView.Telemetry.Counters.GBufferDebugFailures;
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
				GBufferPassInfo.DepthStencilRenderTarget = SceneTargets->Depth;
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
					PreparedView.Receiver.StaticMeshes,
					PreparedView.ResolvedReceiver.StaticMeshes
				);
				const FGeometryExecutionResult SkeletalResult = SkeletalMeshRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					PreparedView.Receiver.SkeletalMeshes,
					PreparedView.ResolvedReceiver.SkeletalMeshes
				);
				const FGeometryExecutionResult TerrainResult = TerrainRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					PreparedView.Receiver.Terrains,
					PreparedView.ResolvedReceiver.Terrains
				);
				CommandList.EndRenderPass();
				Result.Targets = GBufferTargets;
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
						SceneTargets->Depth
					);
				}
				++PreparedView.Telemetry.Counters.GBufferEnabledViews;
				PreparedView.Telemetry.Counters.GBufferAttachmentBytes =
					FGBufferRenderer::CalculateTargetBytes(Width, Height);
				PreparedView.Telemetry.Counters.GBufferAttemptedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferAttemptedDraws
					+ PreparedView.ResolvedReceiver.SkeletalMeshes.GBufferAttemptedDraws
					+ PreparedView.ResolvedReceiver.Terrains.GBufferAttemptedDraws;
				PreparedView.Telemetry.Counters.GBufferSuccessfulDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferSuccessfulDraws
					+ PreparedView.ResolvedReceiver.SkeletalMeshes.GBufferSuccessfulDraws
					+ PreparedView.ResolvedReceiver.Terrains.GBufferSuccessfulDraws;
				PreparedView.Telemetry.Counters.GBufferRejectedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferRejectedDraws
					+ PreparedView.ResolvedReceiver.SkeletalMeshes.GBufferRejectedDraws
					+ PreparedView.ResolvedReceiver.Terrains.GBufferRejectedDraws;
				PreparedView.Telemetry.Counters.GBufferSkippedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferSkippedDraws
					+ PreparedView.ResolvedReceiver.SkeletalMeshes.GBufferSkippedDraws
					+ PreparedView.ResolvedReceiver.Terrains.GBufferSkippedDraws;
				PreparedView.Telemetry.Counters.GBufferStaticMeshAttemptedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferLocalAttemptedDraws;
				PreparedView.Telemetry.Counters.GBufferStaticMeshSuccessfulDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferLocalSuccessfulDraws;
				PreparedView.Telemetry.Counters.GBufferStaticMeshRejectedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferLocalRejectedDraws;
				PreparedView.Telemetry.Counters.GBufferStaticMeshSkippedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferLocalSkippedDraws;
				PreparedView.Telemetry.Counters.GBufferSplineMeshAttemptedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferSplineAttemptedDraws;
				PreparedView.Telemetry.Counters.GBufferSplineMeshSuccessfulDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferSplineSuccessfulDraws;
				PreparedView.Telemetry.Counters.GBufferSplineMeshRejectedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferSplineRejectedDraws;
				PreparedView.Telemetry.Counters.GBufferSplineMeshSkippedDraws =
					PreparedView.ResolvedReceiver.StaticMeshes.GBufferSplineSkippedDraws;
				PreparedView.Telemetry.Counters.GBufferSkeletalMeshAttemptedDraws =
					PreparedView.ResolvedReceiver.SkeletalMeshes.GBufferAttemptedDraws;
				PreparedView.Telemetry.Counters.GBufferSkeletalMeshSuccessfulDraws =
					PreparedView.ResolvedReceiver.SkeletalMeshes.GBufferSuccessfulDraws;
				PreparedView.Telemetry.Counters.GBufferSkeletalMeshRejectedDraws =
					PreparedView.ResolvedReceiver.SkeletalMeshes.GBufferRejectedDraws;
				PreparedView.Telemetry.Counters.GBufferSkeletalMeshSkippedDraws =
					PreparedView.ResolvedReceiver.SkeletalMeshes.GBufferSkippedDraws;
				PreparedView.Telemetry.Counters.GBufferTerrainAttemptedDraws =
					PreparedView.ResolvedReceiver.Terrains.GBufferAttemptedDraws;
				PreparedView.Telemetry.Counters.GBufferTerrainSuccessfulDraws =
					PreparedView.ResolvedReceiver.Terrains.GBufferSuccessfulDraws;
				PreparedView.Telemetry.Counters.GBufferTerrainRejectedDraws =
					PreparedView.ResolvedReceiver.Terrains.GBufferRejectedDraws;
				PreparedView.Telemetry.Counters.GBufferTerrainSkippedDraws =
					PreparedView.ResolvedReceiver.Terrains.GBufferSkippedDraws;
			}
		}
		return Result;
	}

	auto FSceneRenderer::RenderGroundTruthAmbientOcclusion_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneRenderPlan& PreparedView,
		FGBufferRenderer::FTargets* GBufferTargets,
		FPostProcessRenderer::FSceneTargets* SceneTargets,
		FDeferredDirectionalLightingRenderer::FRenderParameters& DeferredParameters,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsGroundTruthAmbientOcclusion,
		bool bGBufferComplete,
		FRHITexture* GroundTruthAmbientOcclusionFallback
	) -> void
	{
		const FSceneView& RenderView = PreparedView.Context.View;
		if (bWantsGroundTruthAmbientOcclusion)
		{
			++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionAttemptedViews;
			auto* AmbientOcclusionTargets = bGBufferComplete ? GroundTruthAmbientOcclusionRenderer.EnsureTargets_RenderThread(
																   Width, Height,
																   RenderView.Settings.AmbientOcclusion.Quality
															   ) :
															   nullptr;
			PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionRetainedBytes =
				GroundTruthAmbientOcclusionRenderer.GetRetainedTargetBytes_RenderThread();
			if (AmbientOcclusionTargets == nullptr)
			{
				++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionUnavailableViews;
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
							SceneTargets->Depth, RenderView
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
							SceneTargets->Depth, RenderView
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
								SceneTargets->Depth, RenderView
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
						FRHITexture* RawDiagnosticTexture =
							AmbientOcclusionTargets->Raw;
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
									SceneTargets->Depth, RenderView
								);
							std::swap(
								AmbientOcclusionTargets->Raw,
								AmbientOcclusionTargets->Scratch
							);
							if (bRawDiagnosticRendered)
								RawDiagnosticTexture =
									AmbientOcclusionTargets->Scratch;
						}
						DeferredParameters.GroundTruthAmbientOcclusionRaw =
							RawDiagnosticTexture;
						DeferredParameters.GroundTruthAmbientOcclusionFiltered =
							AmbientOcclusionTargets->Raw;
						DeferredParameters.GroundTruthAmbientOcclusionResolved =
							AmbientOcclusionTargets->Quality
									== EGroundTruthAmbientOcclusionQuality::HalfResolution ?
								AmbientOcclusionTargets->Resolved.GetReference() :
								AmbientOcclusionTargets->Raw.GetReference();
						DeferredParameters.GroundTruthAmbientOcclusionSelector =
							AmbientOcclusionTargets->Selector != nullptr ? AmbientOcclusionTargets->Selector.GetReference() : GroundTruthAmbientOcclusionFallback;
						DeferredParameters.bGroundTruthAmbientOcclusionEnabled = true;
						DeferredParameters.bGroundTruthAmbientOcclusionHalfResolution =
							AmbientOcclusionTargets->Quality
							== EGroundTruthAmbientOcclusionQuality::HalfResolution;
						++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionEnabledViews;
						if (AmbientOcclusionTargets->Quality
							== EGroundTruthAmbientOcclusionQuality::HalfResolution)
							++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionHalfResolutionViews;
						else
							++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionFullResolutionViews;
						PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionActiveBytes =
							FGroundTruthAmbientOcclusionRenderer::
								CalculateTargetBytes(Width, Height, AmbientOcclusionTargets->Quality);
						if (Options.GroundTruthAmbientOcclusionDebugMode
							!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
						{
							++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionDebugViews;
						}
						FilterTiming.Commit();
						if (CaptureSink != nullptr)
							CaptureSink(
								CommandList, AmbientOcclusionTargets->Raw, true
							);
					}
					else if (!bFiltered)
					{
						++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionFilterPassFailures;
					}
					else
					{
						++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionResolvePassFailures;
					}
				}
				else
				{
					++PreparedView.Telemetry.Counters.GroundTruthAmbientOcclusionRawPassFailures;
				}
			}
		}
	}

	auto FSceneRenderer::RenderContactShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneRenderPlan& PreparedView,
		FGBufferRenderer::FTargets* GBufferTargets,
		FPostProcessRenderer::FSceneTargets* SceneTargets,
		FDeferredDirectionalLightingRenderer::FRenderParameters& DeferredParameters,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete,
		bool bGBufferHasGeometry
	) -> void
	{
		const FSceneView& RenderView = PreparedView.Context.View;
		const bool bWantsContactVisibility = bWantsProductionDeferred
											 && RenderView.Settings.DirectionalShadow.bEnableContactShadows
											 && PreparedView.DirectionalShadow
											 && PreparedView.DirectionalShadow->View.bEnabled;
		if (bWantsContactVisibility && bGBufferComplete
			&& bGBufferHasGeometry)
		{
			const EContactShadowRoutePreference RoutePreference =
				RenderView.Settings.DirectionalShadow.ContactRoutePreference;
			const bool bForceFragment = Options.bForceFragmentContactVisibility
										|| RoutePreference == EContactShadowRoutePreference::Fragment;
			const bool bForceCompute = !Options.bForceFragmentContactVisibility
									   && RoutePreference == EContactShadowRoutePreference::Compute;
			auto* FragmentContactTargets = bForceCompute ? nullptr : ContactShadowRenderer.EnsureTargets_RenderThread(Width, Height);
			auto* ComputeContactTargets = bForceFragment ? nullptr : ContactShadowRenderer.EnsureComputeTargets_RenderThread(Width, Height);
			PreparedView.Telemetry.Counters.ContactShadowRetainedBytes =
				ContactShadowRenderer.GetRetainedTargetBytes_RenderThread();
			const auto ContactResult = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, FragmentContactTargets, ComputeContactTargets,
				GBufferTargets->Material, GBufferTargets->Normals,
				GBufferTargets->Surface, GBufferTargets->Emissive,
				SceneTargets->Depth, RenderView,
				PreparedView.DirectionalShadow->View.LightDirection, Width, Height
			);
			const size_t ReasonIndex = static_cast<size_t>(ContactResult.Reason);
			if (ReasonIndex < PreparedView.Telemetry.Counters.ContactShadowRouteReasons.size())
				++PreparedView.Telemetry.Counters.ContactShadowRouteReasons[ReasonIndex];
			if (ContactResult.Visibility != nullptr)
			{
				PreparedView.Telemetry.Counters.ContactShadowActiveBytes =
					FContactShadowVisibilityRenderer::CalculateTargetBytes(Width, Height);
				DeferredParameters.ContactVisibility = ContactResult.Visibility;
				DeferredParameters.bContactVisibilityEnabled = true;
				DeferredParameters.bContactVisibilityDebug =
					RenderView.Settings.DirectionalShadow.bShowContactDebug;
				++PreparedView.Telemetry.Counters.ContactShadowEnabledViews;
				if (ContactResult.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute)
				{
					++PreparedView.Telemetry.Counters.ContactShadowComputeViews;
					++PreparedView.Telemetry.Counters.ContactShadowDispatches;
				}
				else
				{
					++PreparedView.Telemetry.Counters.ContactShadowFragmentViews;
					++PreparedView.Telemetry.Counters.ContactShadowDraws;
				}
			}
			else
			{
				++PreparedView.Telemetry.Counters.ContactShadowPassFailures;
				++PreparedView.Telemetry.Counters.ContactShadowFactorOneViews;
			}
		}
	}

	auto FSceneRenderer::RenderVolumetricCloudShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneRenderPlan& PreparedView,
		FPostProcessRenderer::FSceneTargets* SceneTargets,
		FDeferredDirectionalLightingRenderer::FRenderParameters& DeferredParameters,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete
	) -> void
	{
		const FPreparedVolumetricCloud* Cloud = PreparedView.VolumetricCloud
			? &*PreparedView.VolumetricCloud : nullptr;
		const bool bRequested = bWantsProductionDeferred && bGBufferComplete
								&& Cloud != nullptr
								&& !PreparedView.Lighting.Lights.Directional.empty()
								&& Cloud->Textures.BaseDensity
								&& Cloud->Textures.DetailDensity
								&& Cloud->Textures.DensitySampler
								&& SceneTargets && SceneTargets->Depth;
		if (!bRequested) return;
		const bool bForceFragment =
			Cloud->bForceFragmentForQualification;
		auto* FragmentTargets =
			VolumetricCloudShadowRenderer.EnsureTargets_RenderThread(Width, Height);
		auto* ComputeTargets = bForceFragment ? nullptr : VolumetricCloudShadowRenderer.EnsureComputeTargets_RenderThread(Width, Height);
		const auto QualityTier = CanonicalizeFixedFrameCloudQuality(
			PreparedView.Context.View.Settings.VolumetricCloud.Quality);
		FRHITexture* Weather = Cloud->Textures.Weather;
		if (!Weather) Weather = DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		const auto Result = VolumetricCloudShadowRenderer.Render_RenderThread(
			CommandList, FragmentTargets, ComputeTargets,
			{.bRequested = true,
			 .BaseDensity = Cloud->Textures.BaseDensity,
			 .DetailDensity = Cloud->Textures.DetailDensity,
			 .Weather = Weather,
			 .SceneDepth = SceneTargets->Depth,
			 .DensitySampler = Cloud->Textures.DensitySampler,
			 .Parameters = Cloud->Parameters,
			 .View = &PreparedView.Context.View,
			 .QualityTier = QualityTier,
			 .Width = Width,
			 .Height = Height}
		);
		auto& Counters = PreparedView.Telemetry.Counters;
		const size_t ReasonIndex = static_cast<size_t>(Result.Reason);
		if (ReasonIndex < Counters.VolumetricCloudShadowRouteReasons.size())
			++Counters.VolumetricCloudShadowRouteReasons[ReasonIndex];
		Counters.VolumetricCloudShadowRetainedBytes =
			VolumetricCloudShadowRenderer.GetRetainedTargetBytes_RenderThread();
		if (!Result.Visibility)
		{
			++Counters.VolumetricCloudShadowFactorOneViews;
			return;
		}
		DeferredParameters.VolumetricCloudVisibility = Result.Visibility;
		PreparedView.VolumetricCloudShadowVisibility = Result.Visibility;
		DeferredParameters.bVolumetricCloudVisibilityEnabled = true;
		Counters.VolumetricCloudShadowActiveBytes = Result.TargetBytes;
		Counters.VolumetricCloudShadowSamples = static_cast<uint64>(Width)
												* Height * Result.SampleCount;
		++Counters.VolumetricCloudShadowEnabledViews;
		if (Result.Route == FVolumetricCloudShadowRenderer::ERoute::Compute)
		{
			++Counters.VolumetricCloudShadowComputeViews;
			++Counters.VolumetricCloudShadowDispatches;
		}
		else
		{
			++Counters.VolumetricCloudShadowFragmentViews;
			++Counters.VolumetricCloudShadowDraws;
		}
	}

	auto FSceneRenderer::RenderIsolatedDeferred_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneRenderPlan& PreparedView,
		FDeferredDirectionalLightingRenderer::FRenderParameters& DeferredParameters,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsIsolatedDeferred,
		bool bGBufferComplete
	) -> FRHITexture*
	{
		FRHITexture* GroundTruthAmbientOcclusionDebugOutput = nullptr;
		if (bWantsIsolatedDeferred)
		{
			auto* DeferredTargets = bGBufferComplete ? DeferredDirectionalLightingRenderer.EnsureTargets_RenderThread(
														   Width, Height
													   ) :
													   nullptr;
			if (DeferredTargets == nullptr)
				++PreparedView.Telemetry.Counters.DeferredDirectionalUnavailableViews;
			else
			{
				DeferredParameters.GroundTruthAmbientOcclusionDebugMode =
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
						CommandList, *DeferredTargets, DeferredParameters
					);
				DeferredTiming.End();
				if (bRendered)
				{
					++PreparedView.Telemetry.Counters.DeferredDirectionalEnabledViews;
					PreparedView.Telemetry.Counters.DeferredDirectionalOutputBytes =
						FDeferredDirectionalLightingRenderer::
							CalculateTargetBytes(Width, Height);
					if (Options.DeferredDirectionalDebugMode
						!= EDeferredDirectionalDebugMode::Disabled)
					{
						++PreparedView.Telemetry.Counters.DeferredDirectionalDebugViews;
					}
					DeferredTiming.Commit();
					const FDeferredDirectionalCaptureSink CaptureSink =
						GetDeferredDirectionalCaptureSink();
					if (CaptureSink != nullptr)
						CaptureSink(CommandList, DeferredTargets->Color);
					if (Options.GroundTruthAmbientOcclusionDebugMode
						!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
					{
						GroundTruthAmbientOcclusionDebugOutput =
							DeferredTargets->Color;
					}
				}
				else
				{
					++PreparedView.Telemetry.Counters.DeferredDirectionalPassFailures;
				}
			}
		}
		return GroundTruthAmbientOcclusionDebugOutput;
	}

	auto FSceneRenderer::RenderPostProcess_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneRenderPlan& PreparedView,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FPostProcessRenderer::FSceneTargets* SceneTargets,
		FGBufferRenderer::FTargets* GBufferTargets,
		FRHITexture* SceneColor,
		FRHITexture* GroundTruthAmbientOcclusionDebugOutput
	) -> ERenderViewResult
	{
		const FSceneView& RenderView = PreparedView.Context.View;
		const uint32 Width = OutputTarget->GetSizeX();
		const uint32 Height = OutputTarget->GetSizeY();
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		FRHITexture* PostProcessInput = SceneColor;
		if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled
			&& GBufferTargets != nullptr)
		{
			auto* DebugTargets =
				GBufferDebugRenderer.EnsureTargets_RenderThread(Width, Height);
			if (DebugTargets != nullptr
				&& GBufferDebugRenderer.Render_RenderThread(
					CommandList,
					GBufferTargets->Material,
					GBufferTargets->Normals,
					GBufferTargets->Surface,
					GBufferTargets->Emissive,
					SceneTargets->Depth,
					DebugTargets->Color,
					RenderView,
					Options.GBufferDebugMode,
					Width,
					Height
				))
			{
				PostProcessInput = DebugTargets->Color;
				++PreparedView.Telemetry.Counters.GBufferDebugViews;
			}
			else
			{
				++PreparedView.Telemetry.Counters.GBufferDebugFailures;
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

		const RendererEditorAssistance::FRequest EditorAssistanceRequest =
			FEditorAssistanceRenderer::AnalyzeRequest(RenderView, ViewportOutput);
		RendererEditorAssistance::FPrepared PreparedEditorAssistance;
		if (!EditorAssistanceRequest.IsEmpty())
		{
			PreparedEditorAssistance =
				EditorAssistanceRenderer.Prepare_RenderThread(
					CommandList,
					RenderView,
					EditorAssistanceRequest
				);
		}
		const bool bHasEditorAssistance =
			PreparedEditorAssistance.HasDrawableOperation();

		FRHIRenderPassInfo PostProcessPassInfo{};
		PostProcessPassInfo.RenderTargetLayout = bHasEditorAssistance ? RenderTargetLayouts::MakeScenePostProcessOutput() : RenderTargetLayouts::MakeFinalScenePostProcessOutput(ViewportOutput);
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
			bHasEditorAssistance,
			RenderView.Settings.PostProcess.ExposureEV
		);
		CommandList.EndRenderPass();
		PostProcessTiming.Commit();
		if (!bHasEditorAssistance)
		{
			return ERenderViewResult::Success;
		}

		FRHIRenderPassInfo EditorAssistancePassInfo{};
		EditorAssistancePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeEditorAssistanceOutput(ViewportOutput);
		EditorAssistancePassInfo.ColorRenderTargets[0] = OutputTarget;
		EditorAssistancePassInfo.DepthStencilRenderTarget =
			SceneTargets->Depth;
		CommandList.BeginRenderPass(
			EditorAssistancePassInfo,
			bPresentOutput ? "EditorAssistancePresentRenderPass" : "EditorAssistanceOffscreenRenderPass"
		);
		EditorAssistanceRenderer.Draw_RenderThread(
			CommandList,
			RenderView,
			PreparedEditorAssistance
		);
		CommandList.EndRenderPass();
		return ERenderViewResult::Success;
	}

	auto FSceneRenderer::RenderVolumetricCloud_RenderThread(
		FRHICommandListImmediate& CommandList, FSceneRenderPlan& PreparedView, FRHITexture* SceneColor, FRHITexture* Depth
	) -> FRHITexture*
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = PreparedView.Context.View;
		const uint32 Width = SceneColor != nullptr ? SceneColor->GetSizeX() : 0;
		const uint32 Height = SceneColor != nullptr ? SceneColor->GetSizeY() : 0;
		const FPreparedVolumetricCloud* Cloud = PreparedView.VolumetricCloud
			? &*PreparedView.VolumetricCloud : nullptr;
		const bool bInputsPresent = Cloud != nullptr
									&& Cloud->Textures.BaseDensity != nullptr
									&& Cloud->Textures.DetailDensity != nullptr
									&& Cloud->Textures.DensitySampler != nullptr
									&& Depth != nullptr;
		const auto QualityTier = CanonicalizeFixedFrameCloudQuality(
			View.Settings.VolumetricCloud.Quality);
		const auto Quality = FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(
			QualityTier
		);
		const auto CloudExtent = FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
			Width, Height, Quality
		);
		auto* FragmentTargets = bInputsPresent ? VolumetricCloudRenderer.EnsureTargets_RenderThread(
																							   CloudExtent.Width, CloudExtent.Height
																						   ) :
																						   nullptr;
		auto* ComputeTargets = bInputsPresent
									   && !Cloud->bForceFragmentForQualification ?
								   VolumetricCloudRenderer.EnsureComputeTargets_RenderThread(
									   CloudExtent.Width, CloudExtent.Height
								   ) :
								   nullptr;
		auto Textures = Cloud != nullptr
			? Cloud->Textures : FVolumetricCloudRenderer::FTextureBindings{};
		Textures.SceneDepth = Depth;
		const FVolumetricCloudRenderer::FRenderResult Result =
			VolumetricCloudRenderer.Render_RenderThread(CommandList, FragmentTargets, ComputeTargets, {.bRequested = Cloud != nullptr, .Textures = Textures, .Parameters = Cloud != nullptr ? Cloud->Parameters : FVolumetricCloudRenderer::FParameters{}, .View = &View, .QualityTier = QualityTier, .SuccessfulSequence = PreparedView.Context.TemporalContext.SuccessfulSequence, .Width = CloudExtent.Width, .Height = CloudExtent.Height, .OutputWidth = Width, .OutputHeight = Height});
		auto& Counters = PreparedView.Telemetry.Counters;
		const auto RouteIndex = static_cast<size_t>(Result.Counters.Reason);
		if (RouteIndex < Counters.VolumetricCloudRouteReasons.size())
			++Counters.VolumetricCloudRouteReasons[RouteIndex];
		Counters.VolumetricCloudDispatches += Result.Counters.Dispatches;
		Counters.VolumetricCloudDraws += Result.Counters.Draws;
		Counters.VolumetricCloudPrimarySamples += Result.Counters.PrimarySamples;
		Counters.VolumetricCloudLightSamples += Result.Counters.LightSamples;
		Counters.VolumetricCloudTargetWidth = Result.Counters.TargetWidth;
		Counters.VolumetricCloudTargetHeight = Result.Counters.TargetHeight;
		Counters.VolumetricCloudOutputWidth = Result.Counters.OutputWidth;
		Counters.VolumetricCloudOutputHeight = Result.Counters.OutputHeight;
		Counters.VolumetricCloudActiveBytes = Result.Counters.TargetBytes;
		if (Result.Counters.Route == FVolumetricCloudRenderer::ERoute::Compute)
			++Counters.VolumetricCloudComputeViews;
		else if (Result.Counters.Route == FVolumetricCloudRenderer::ERoute::Fragment)
			++Counters.VolumetricCloudFragmentViews;
		else
			++Counters.VolumetricCloudDisabledViews;
		const FVolumetricCloudRenderer::FTemporalReconstructionResult Temporal =
			Result.Cloud != nullptr ? VolumetricCloudRenderer.ReconstructTemporal_RenderThread(
										  CommandList, {.CurrentCloud = Result.Cloud, .View = &View, .TemporalContext = &PreparedView.Context.TemporalContext, .ViewState = PreparedView.Context.ViewState, .Parameters = Cloud != nullptr ? Cloud->Parameters : FVolumetricCloudRenderer::FParameters{}, .QualityTier = QualityTier, .CloudHistoryKey = Cloud != nullptr ? Cloud->HistoryKey : 0}
									  ) :
									  FVolumetricCloudRenderer::FTemporalReconstructionResult{};
		Counters.VolumetricCloudHistoryBytes = Temporal.HistoryBytes;
		if (Temporal.bCandidatePublished)
			++Counters.VolumetricCloudTemporalDraws;
		if (Temporal.bHistoryAccepted)
			++Counters.VolumetricCloudHistoryAccepted;
		else if (Temporal.bCandidatePublished)
			++Counters.VolumetricCloudHistoryRejected;
		FRHITexture* Composite = Temporal.Cloud != nullptr ? VolumetricCloudRenderer.Composite_RenderThread(
																 CommandList, SceneColor, Temporal.Cloud, Depth,
																 PreparedView.VolumetricCloudShadowVisibility,
																 Temporal.bCandidatePublished,
																 Temporal.bHistoryAccepted, View
															 ) :
															 nullptr;
		Counters.VolumetricCloudRetainedBytes =
			VolumetricCloudRenderer.GetRetainedTargetBytes_RenderThread();
		if (Composite != nullptr)
		{
			++Counters.VolumetricCloudEnabledViews;
			++Counters.VolumetricCloudCompositeDraws;
			return Composite;
		}
		return SceneColor;
	}

	auto FSceneRenderer::RenderScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneRenderPlan& PreparedView,
		FRHITexture*& SceneColor,
		FRHITexture* Depth,
		const FDeferredDirectionalLightingRenderer::FRenderParameters*
			DeferredParameters
	) -> ERenderViewResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = PreparedView.Context.View;
		if (SceneColor == nullptr || Depth == nullptr)
			return ERenderViewResult::RendererResourcesUnavailable;
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
				CommandList, PreparedView, SceneColor
			);
			CommandList.EndRenderPass();
			return bRendered ? ERenderViewResult::Success : ERenderViewResult::RequiredEnvironmentUnavailable;
		}
		if (DeferredParameters == nullptr)
		{
			++PreparedView.Telemetry.Counters.HybridDeferredUnavailableViews;
			return ERenderViewResult::RendererResourcesUnavailable;
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
		if (PreparedView.Environment)
		{
			if (PreparedView.Environment->Texture != nullptr)
			{
				bBootstrapRendered = SkyBoxRenderer.DrawTexture_RenderThread(
					CommandList, View, PreparedView.Environment->Texture,
					PreparedView.Environment->SkyBox, true
				);
			}
			else
			{
				SkyBoxRenderer.Draw_RenderThread(
					CommandList, View, PreparedView.Environment->SkyBox, true
				);
			}
		}
		CommandList.EndRenderPass();
		if (!bBootstrapRendered)
			return ERenderViewResult::RequiredEnvironmentUnavailable;

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
			++PreparedView.Telemetry.Counters.HybridDeferredUnavailableViews;
			return ERenderViewResult::RendererResourcesUnavailable;
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
			const auto& StaticDraws = Pass == EMeshBasePass::Opaque ? PreparedView.Receiver.StaticMeshes.Opaque : PreparedView.Receiver.StaticMeshes.Masked;
			for (const FPreparedStaticMeshDraw& Draw : StaticDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, PreparedView.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						PreparedView.Receiver.StaticMeshes,
						PreparedView.ResolvedReceiver.StaticMeshes, true
					);
				}
			const auto& SkeletalDraws = Pass == EMeshBasePass::Opaque ? PreparedView.Receiver.SkeletalMeshes.Opaque : PreparedView.Receiver.SkeletalMeshes.Masked;
			for (const FPreparedSkeletalMeshDraw& Draw : SkeletalDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, PreparedView.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						PreparedView.Receiver.SkeletalMeshes,
						PreparedView.ResolvedReceiver.SkeletalMeshes, true
					);
				}
			const auto& TerrainDraws = Pass == EMeshBasePass::Opaque ? PreparedView.Receiver.Terrains.Opaque : PreparedView.Receiver.Terrains.Masked;
			for (const FPreparedTerrainDraw& Draw : TerrainDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					TerrainRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, PreparedView.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Draw,
						PreparedView.Receiver.Terrains,
						PreparedView.ResolvedReceiver.Terrains,
						true
					);
				}
		}
		CommandList.EndRenderPass();
		RetainedOpaqueTiming.Commit();
		const FVolumetricCloudTimingQuerySink VolumetricCloudTimingSink =
			GetVolumetricCloudTimingQuerySink();
		TScopedRendererGPUTimingQuery VolumetricCloudTiming(
			CommandList, VolumetricCloudTimingSink
		);
		const std::array CloudBoundaryTransitions{
			FRHITextureTransition::Whole(Depth, ERHIAccess::DepthStencilReadWrite, ERHIAccess::GraphicsShaderRead)
		};
		CommandList.TransitionTextures(CloudBoundaryTransitions);

		SceneColor = RenderVolumetricCloud_RenderThread(
			CommandList, PreparedView, SceneColor, Depth
		);
		const std::array SortedTranslucencyTransitions{
			FRHITextureTransition::Whole(SceneColor, ERHIAccess::GraphicsShaderRead, ERHIAccess::ColorAttachmentReadWrite)
		};
		CommandList.TransitionTextures(SortedTranslucencyTransitions);
		VolumetricCloudTiming.Commit();
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
			 PreparedView.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					PreparedView.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.StaticMeshes,
					PreparedView.ResolvedReceiver.StaticMeshes, true
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					PreparedView.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.SkeletalMeshes,
					PreparedView.ResolvedReceiver.SkeletalMeshes, true
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					PreparedView.Receiver.Terrains.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.Terrains,
					PreparedView.ResolvedReceiver.Terrains, true
				);
		}
		CommandList.EndRenderPass();
		SortedTranslucencyTiming.Commit();
		// Lit opaque/masked sections were already consumed by GBuffer + deferred
		// lighting, so the retained-forward attempted count intentionally does not
		// equal every prepared section as it does in the all-forward finalizer.
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			PreparedView.ResolvedReceiver.StaticMeshes);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			PreparedView.ResolvedReceiver.SkeletalMeshes);
		TerrainRenderer.FinalizeExecution_RenderThread(
			PreparedView.ResolvedReceiver.Terrains);
		++PreparedView.Telemetry.Counters.HybridDeferredEnabledViews;
		return ERenderViewResult::Success;
	}

	auto FSceneRenderer::RenderSpecialForwardScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		FSceneRenderPlan& PreparedView,
		FRHITexture* RenderTarget
	) -> bool
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderScene");
		const FSceneView& View = PreparedView.Context.View;
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

		if (PreparedView.Environment)
		{
			if (PreparedView.Environment->Texture != nullptr)
			{
				if (!SkyBoxRenderer.DrawTexture_RenderThread(
						CommandList,
						View,
						PreparedView.Environment->Texture,
						PreparedView.Environment->SkyBox
					))
				{
					return false;
				}
			}
			else
			{
				SkyBoxRenderer.Draw_RenderThread(
					CommandList, View, PreparedView.Environment->SkyBox
				);
			}
		}

		for (const EMeshBasePass Pass : {
				 EMeshBasePass::Opaque, EMeshBasePass::Masked
			 })
		{
			StaticMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				PreparedView.Receiver.StaticMeshes,
				PreparedView.ResolvedReceiver.StaticMeshes
			);
			SkeletalMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				PreparedView.Receiver.SkeletalMeshes,
				PreparedView.ResolvedReceiver.SkeletalMeshes
			);
			TerrainRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				PreparedView.Receiver.Terrains,
				PreparedView.ResolvedReceiver.Terrains
			);
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 PreparedView.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					PreparedView.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.StaticMeshes,
					PreparedView.ResolvedReceiver.StaticMeshes
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					PreparedView.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.SkeletalMeshes,
					PreparedView.ResolvedReceiver.SkeletalMeshes
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					PreparedView.Receiver.Terrains.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.Terrains,
					PreparedView.ResolvedReceiver.Terrains
				);
		}
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			PreparedView.ResolvedReceiver.StaticMeshes
		);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			PreparedView.ResolvedReceiver.SkeletalMeshes
		);
		TerrainRenderer.FinalizeExecution_RenderThread(
			PreparedView.ResolvedReceiver.Terrains);
		return true;
	}
} // namespace Durin
