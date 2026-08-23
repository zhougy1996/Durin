#include "Renderers/FixedSceneFrameExecutor.h"

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

	FFixedSceneFrameExecutor::FFixedSceneFrameExecutor(FSceneRenderer& Renderer)
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
		, ViewStates(Renderer.ViewStates)
		, RenderSubmissionSerial(Renderer.RenderSubmissionSerial)
		, Qualification(GetRendererQualificationPolicy())
	{
	}

	auto FFixedSceneFrameExecutor::Execute_RenderThread(
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
		Telemetry = {};
		ResolvedFrame = {};
		TemporalContext = {};
		ViewState = nullptr;
		if (RenderSubmissionSerial != std::numeric_limits<uint64>::max())
			++RenderSubmissionSerial;
		Telemetry.View.VolumetricCloud.VolumetricCloudQuality =
			CanonicalizeFixedFrameCloudQuality(View.Settings.VolumetricCloud.Quality);
		Telemetry.View.VolumetricCloud.VolumetricCloudDebugMode =
			CanonicalizeFixedFrameCloudDebugMode(View.Settings.VolumetricCloud.DebugMode);
		FSceneTelemetryPublication TelemetryPublication(
			Telemetry, OutStatistics
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
		FSceneView RenderView = FSceneRenderer::FitViewToOutput(
			View, Width, Height);
		ViewState = ViewStates.Find(RenderView.ViewStateId);
		if (ViewState != nullptr && ViewState->IsSubmissionActive())
		{
			TemporalContext.Current =
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				);
			TemporalContext.SubmissionSerial =
				RenderSubmissionSerial;
			TemporalContext.Discontinuities =
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
			TemporalContext = ViewState->Begin(
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				),
				RenderSubmissionSerial, RenderView.bDiscardHistory
			);
		}
		else if (TemporalContext.Discontinuities
				 != ESceneViewDiscontinuity::DuplicateSubmission)
		{
			TemporalContext.Current =
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				);
			TemporalContext.SubmissionSerial =
				RenderSubmissionSerial;
			TemporalContext.Discontinuities =
				ESceneViewDiscontinuity::MissingState;
			if (RenderView.ViewStateId.IsValid())
			{
				ReportFixedFrameRejectedViewState(
					"a missing, released, or foreign",
					RenderView.ViewStateId
				);
			}
		}
		FSceneFramePreparationResult Preparation = PrepareView_RenderThread(
			CommandList, Scene, RenderView, Options);
		if (!Preparation.IsSuccess()) return Preparation.Result;
		const FSceneRenderPlan PreparedView = std::move(*Preparation.Plan);
		const ERenderViewResult ResolutionResult =
			ResolveFrameResources_RenderThread(CommandList, PreparedView);
		if (ResolutionResult != ERenderViewResult::Success)
			return ResolutionResult;
		const FSceneFrameRequirements Requirements = BuildFrameRequirements(
			PreparedView, Options, Width, Height);
		const ERenderViewResult TargetResolutionResult =
			ResolveFrameTargets_RenderThread(Requirements);
		if (TargetResolutionResult != ERenderViewResult::Success)
			return TargetResolutionResult;
		const auto& SceneTargets = *ResolvedFrame.Targets.Scene;
		FSceneFrameOutcome Outcome;
		Outcome.DirectionalShadow = RenderDirectionalShadow_RenderThread(
			CommandList, PreparedView);
		const bool bRequiresDeferredOpaque =
			RenderView.Settings.Mode.RenderMode == ERenderMode::Lit
			&& RenderView.Settings.Mode.RasterMode == ERasterMode::Solid;
		const bool bWantsIsolatedDeferred =
			Qualification.bEnableDeferredDirectional
			|| Options.DeferredDirectionalDebugMode
				   != EDeferredDirectionalDebugMode::Disabled
			|| Options.GroundTruthAmbientOcclusionDebugMode
				   != EGroundTruthAmbientOcclusionDebugMode::Disabled;
		const bool bWantsIsolatedGroundTruthAmbientOcclusion =
			Qualification.bEnableGroundTruthAmbientOcclusion
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
					ResolvedFrame.Receiver.StaticMeshes
				)
				&& SkeletalMeshRenderer.PrepareHybridRetainedResources_RenderThread(
					PreparedView.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes
				)
				&& TerrainRenderer.PrepareHybridRetainedResources_RenderThread(
					CommandList, PreparedView.Receiver.Terrains,
					ResolvedFrame.Receiver.Terrains
				));
		const bool bNeedsGBuffer = Qualification.bEnableGBuffer
								   || Options.GBufferDebugMode != EGBufferDebugMode::Disabled
								   || bWantsDeferredInputs;
		Outcome.GBuffer = RenderGBuffer_RenderThread(
			CommandList, PreparedView, SceneTargets, Options, Width, Height,
			bNeedsGBuffer, bWantsIsolatedDeferred
		);
		const auto* GBufferTargets = Outcome.GBuffer.Targets;
		const bool bGBufferComplete = Outcome.GBuffer.IsComplete();
		Outcome.AmbientOcclusion =
			RenderGroundTruthAmbientOcclusion_RenderThread(
				CommandList, PreparedView, GBufferTargets, SceneTargets,
				Options, Width, Height,
				bWantsGroundTruthAmbientOcclusion, bGBufferComplete);
		Outcome.ContactShadow = RenderContactShadows_RenderThread(
			CommandList, PreparedView, GBufferTargets, SceneTargets,
			Options, Width, Height, bWantsProductionDeferred,
			bGBufferComplete, Outcome.GBuffer.bRenderedGeometry);
		Outcome.VolumetricCloudShadow =
			RenderVolumetricCloudShadows_RenderThread(
				CommandList, PreparedView, SceneTargets,
				Width, Height, bWantsProductionDeferred, bGBufferComplete);
		const auto DeferredParameters = bWantsDeferredInputs
			? BuildDeferredParameters(
				PreparedView, Outcome.DirectionalShadow, Outcome.GBuffer,
				Outcome.AmbientOcclusion, Outcome.ContactShadow,
				Outcome.VolumetricCloudShadow, SceneTargets, Options)
			: std::nullopt;
		if (DeferredParameters)
		{
			Outcome.IsolatedDeferred = RenderIsolatedDeferred_RenderThread(
				CommandList, *DeferredParameters, Options,
				Width, Height, bWantsIsolatedDeferred
			);
		}
		else if (bWantsIsolatedDeferred)
		{
			Outcome.IsolatedDeferred.Status = EScenePassStatus::Failed;
			++Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
		}

		const bool bProductionResourcesReady =
			!bWantsProductionDeferred
			|| (bGBufferComplete && bHybridRetainedResourcesReady
				&& DeferredParameters.has_value());
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			ProductionDeferredParameters;
		if (bWantsProductionDeferred && bProductionResourcesReady)
		{
			ProductionDeferredParameters = *DeferredParameters;
			ProductionDeferredParameters->DiagnosticMode = 0;
		}
		const FSceneColorTimingQuerySink SceneColorTimingSink =
			GetSceneColorTimingQuerySink();
		TScopedRendererGPUTimingQuery SceneColorTiming(
			CommandList, SceneColorTimingSink
		);
		Outcome.SceneColor = RenderScene_RenderThread(
			CommandList, PreparedView, SceneTargets.Color, SceneTargets.Depth,
			ProductionDeferredParameters ? &*ProductionDeferredParameters : nullptr,
			Outcome.VolumetricCloudShadow.Visibility
		);
		SceneColorTiming.Commit();
		if (!Outcome.SceneColor.IsSuccess()) return Outcome.SceneColor.Result;
		ReduceStaticMeshTelemetry(
			PreparedView.Receiver.StaticMeshes,
			ResolvedFrame.Receiver.StaticMeshes, Telemetry.View
		);
		ReduceSkeletalMeshTelemetry(
			PreparedView.Receiver.SkeletalMeshes,
			ResolvedFrame.Receiver.SkeletalMeshes,
			ResolvedFrame.Receiver.SkeletalPalettes,
			Telemetry.View
		);
		ReduceTerrainTelemetry(PreparedView.Receiver.Terrains,
			ResolvedFrame.Receiver.Terrains, Telemetry.View);

		Outcome.PostProcess = RenderPostProcess_RenderThread(
			CommandList, PreparedView, View, OutputTarget, bPresentOutput,
			Options, SceneTargets, GBufferTargets, Outcome.SceneColor.SceneColor,
			Outcome.IsolatedDeferred.Output
		);
		if (Outcome.PostProcess.Result == ERenderViewResult::Success)
		{
			ViewStateSubmission.Commit();
			TelemetryPublication.Commit();
		}
		return Outcome.PostProcess.Result;
	}


	auto FFixedSceneFrameExecutor::RenderGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
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
			GBufferTargets = ResolvedFrame.Targets.GBuffer
				? &*ResolvedFrame.Targets.GBuffer : nullptr;
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
					PreparedView.Receiver.StaticMeshes,
					ResolvedFrame.Receiver.StaticMeshes
				);
				const FGeometryExecutionResult SkeletalResult = SkeletalMeshRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					PreparedView.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes
				);
				const FGeometryExecutionResult TerrainResult = TerrainRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					PreparedView.Receiver.Terrains,
					ResolvedFrame.Receiver.Terrains
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

	auto FFixedSceneFrameExecutor::RenderGroundTruthAmbientOcclusion_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsGroundTruthAmbientOcclusion,
		bool bGBufferComplete
	) -> FGroundTruthAmbientOcclusionPassResult
	{
		FGroundTruthAmbientOcclusionPassResult Result;
		const FSceneView& RenderView = PreparedView.Context.View;
		if (bWantsGroundTruthAmbientOcclusion)
		{
			++Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionAttemptedViews;
			std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
				AmbientOcclusionTargetsStorage;
			if (bGBufferComplete
				&& ResolvedFrame.Targets.GroundTruthAmbientOcclusion
			)
			{
				AmbientOcclusionTargetsStorage =
					*ResolvedFrame.Targets.GroundTruthAmbientOcclusion;
			}
			auto* AmbientOcclusionTargets = AmbientOcclusionTargetsStorage
				? &*AmbientOcclusionTargetsStorage : nullptr;
			Telemetry.View.AmbientOcclusion.GroundTruthAmbientOcclusionRetainedBytes =
				GroundTruthAmbientOcclusionRenderer.GetRetainedTargetBytes_RenderThread();
			if (AmbientOcclusionTargets == nullptr)
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
									SceneTargets.Depth, RenderView
								);
							std::swap(
								AmbientOcclusionTargets->Raw,
								AmbientOcclusionTargets->Scratch
							);
							if (bRawDiagnosticRendered)
								RawDiagnosticTexture =
									AmbientOcclusionTargets->Scratch;
						}
						Result.Status = EScenePassStatus::Complete;
						Result.Raw = RawDiagnosticTexture;
						Result.Filtered = AmbientOcclusionTargets->Raw;
						Result.Resolved =
							AmbientOcclusionTargets->Quality
									== EGroundTruthAmbientOcclusionQuality::HalfResolution ?
								AmbientOcclusionTargets->Resolved.GetReference() :
								AmbientOcclusionTargets->Raw.GetReference();
						Result.Selector =
							AmbientOcclusionTargets->Selector.GetReference();
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

	auto FFixedSceneFrameExecutor::RenderContactShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		const FGBufferRenderer::FTargets* GBufferTargets,
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
		const FSceneView& RenderView = PreparedView.Context.View;
		const bool bWantsContactVisibility = bWantsProductionDeferred
											 && RenderView.Settings.DirectionalShadow.bEnableContactShadows
											 && PreparedView.DirectionalShadow
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
			auto* FragmentContactTargets = !bForceCompute
				&& ResolvedFrame.Targets.ContactFragment
				? &*ResolvedFrame.Targets.ContactFragment : nullptr;
			auto* ComputeContactTargets = !bForceFragment
				&& ResolvedFrame.Targets.ContactCompute
				? &*ResolvedFrame.Targets.ContactCompute : nullptr;
			Telemetry.View.ContactShadow.ContactShadowRetainedBytes =
				ContactShadowRenderer.GetRetainedTargetBytes_RenderThread();
			const auto ContactResult = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, FragmentContactTargets, ComputeContactTargets,
				GBufferTargets->Material, GBufferTargets->Normals,
				GBufferTargets->Surface, GBufferTargets->Emissive,
				SceneTargets.Depth, RenderView,
				PreparedView.DirectionalShadow->View.LightDirection, Width, Height
			);
			const size_t ReasonIndex = static_cast<size_t>(ContactResult.Reason);
			if (ReasonIndex < Telemetry.View.ContactShadow.ContactShadowRouteReasons.size())
				++Telemetry.View.ContactShadow.ContactShadowRouteReasons[ReasonIndex];
			if (ContactResult.Visibility != nullptr)
			{
				Telemetry.View.ContactShadow.ContactShadowActiveBytes =
					FContactShadowVisibilityRenderer::CalculateTargetBytes(Width, Height);
				PassResult.Status = EScenePassStatus::Complete;
				PassResult.Visibility = ContactResult.Visibility;
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

	auto FFixedSceneFrameExecutor::RenderVolumetricCloudShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete
	) -> FVolumetricCloudShadowPassResult
	{
		FVolumetricCloudShadowPassResult PassResult;
		const FPreparedVolumetricCloud* Cloud = PreparedView.VolumetricCloud
			? &*PreparedView.VolumetricCloud : nullptr;
		const FResolvedVolumetricCloud* ResolvedCloud =
			ResolvedFrame.VolumetricCloud
				? &*ResolvedFrame.VolumetricCloud : nullptr;
		const bool bRequested = bWantsProductionDeferred && bGBufferComplete
								&& Cloud != nullptr && ResolvedCloud != nullptr
								&& !PreparedView.Lighting.Lights.Directional.empty()
								&& ResolvedCloud->Textures.BaseDensity
								&& ResolvedCloud->Textures.DetailDensity
								&& ResolvedCloud->Textures.DensitySampler
								&& SceneTargets.Depth;
		if (!bRequested) return PassResult;
		PassResult.Status = EScenePassStatus::Failed;
		const bool bForceFragment =
			Qualification.bForceFragmentVolumetricCloud;
		auto* FragmentTargets = ResolvedFrame.Targets.VolumetricCloudShadowFragment
			? &*ResolvedFrame.Targets.VolumetricCloudShadowFragment : nullptr;
		auto* ComputeTargets = !bForceFragment
			&& ResolvedFrame.Targets.VolumetricCloudShadowCompute
			? &*ResolvedFrame.Targets.VolumetricCloudShadowCompute : nullptr;
		const auto QualityTier = CanonicalizeFixedFrameCloudQuality(
			PreparedView.Context.View.Settings.VolumetricCloud.Quality);
		FRHITexture* Weather = ResolvedCloud->Textures.Weather;
		if (!Weather) Weather = DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		const auto Result = VolumetricCloudShadowRenderer.Render_RenderThread(
			CommandList, FragmentTargets, ComputeTargets,
			{.bRequested = true,
				 .BaseDensity = ResolvedCloud->Textures.BaseDensity,
				 .DetailDensity = ResolvedCloud->Textures.DetailDensity,
			 .Weather = Weather,
			 .SceneDepth = SceneTargets.Depth,
				 .DensitySampler = ResolvedCloud->Textures.DensitySampler,
			 .Parameters = Cloud->Parameters,
			 .View = &PreparedView.Context.View,
			 .QualityTier = QualityTier,
			 .Width = Width,
			 .Height = Height}
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
		PassResult.Visibility = Result.Visibility;
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

	auto FFixedSceneFrameExecutor::BuildDeferredParameters(
		const FSceneRenderPlan& PreparedView,
		const FDirectionalShadowPassResult& DirectionalShadow,
		const FGBufferPassResult& GBuffer,
		const FGroundTruthAmbientOcclusionPassResult& AmbientOcclusion,
		const FContactShadowPassResult& ContactShadow,
		const FVolumetricCloudShadowPassResult& CloudShadow,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FSceneViewRenderOptions& Options
	) -> std::optional<
		FDeferredDirectionalLightingRenderer::FRenderParameters>
	{
		if (!GBuffer.IsComplete()) return std::nullopt;
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
		const auto& Targets = *GBuffer.Targets;
		return FDeferredDirectionalLightingRenderer::FRenderParameters{
			.Material = Targets.Material,
			.Normals = Targets.Normals,
			.Surface = Targets.Surface,
			.Emissive = Targets.Emissive,
			.Depth = SceneTargets.Depth,
			.EnvironmentIrradiance = EnvironmentIrradiance,
			.EnvironmentPrefiltered = EnvironmentPrefiltered,
			.EnvironmentBrdfLut = EnvironmentBrdfLut,
			.EnvironmentSampler = EnvironmentSampler,
			.DirectionalShadowTexture = DirectionalShadow.IsComplete()
				? DirectionalShadow.Texture
				: DefaultTextures.GetArray_RenderThread(),
			.DirectionalShadowSampler = DirectionalShadow.Sampler,
			.GroundTruthAmbientOcclusionRaw = AmbientOcclusion.IsComplete()
				? AmbientOcclusion.Raw : White,
			.GroundTruthAmbientOcclusionFiltered =
				AmbientOcclusion.IsComplete() ? AmbientOcclusion.Filtered : White,
			.GroundTruthAmbientOcclusionResolved =
				AmbientOcclusion.IsComplete() ? AmbientOcclusion.Resolved : White,
			.GroundTruthAmbientOcclusionSelector =
				AmbientOcclusion.IsComplete() && AmbientOcclusion.Selector != nullptr
					? AmbientOcclusion.Selector : White,
			.ContactVisibility = ContactShadow.IsComplete()
				? ContactShadow.Visibility : White,
			.VolumetricCloudVisibility = CloudShadow.IsComplete()
				? CloudShadow.Visibility : White,
			.Lighting = ResolvedFrame.Lighting.UniformBuffer,
			.View = &PreparedView.Context.View,
			.DiagnosticMode = static_cast<uint32>(
				Options.DeferredDirectionalDebugMode),
			.bGroundTruthAmbientOcclusionEnabled = AmbientOcclusion.IsComplete(),
			.bGroundTruthAmbientOcclusionHalfResolution =
				AmbientOcclusion.bHalfResolution,
			.bContactVisibilityEnabled = ContactShadow.IsComplete(),
			.bContactVisibilityDebug = ContactShadow.bDebug,
			.bVolumetricCloudVisibilityEnabled = CloudShadow.IsComplete()};
	}

	auto FFixedSceneFrameExecutor::RenderIsolatedDeferred_RenderThread(
		FRHICommandListImmediate& CommandList,
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
			auto* DeferredTargets = ResolvedFrame.Targets.IsolatedDeferred
				? &*ResolvedFrame.Targets.IsolatedDeferred : nullptr;
			if (DeferredTargets == nullptr)
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
						CommandList, *DeferredTargets, Parameters
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
						CaptureSink(CommandList, DeferredTargets->Color);
					if (Options.GroundTruthAmbientOcclusionDebugMode
						!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
					{
						Result.Output = DeferredTargets->Color;
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

	auto FFixedSceneFrameExecutor::RenderPostProcess_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FGBufferRenderer::FTargets* GBufferTargets,
		FRHITexture* SceneColor,
		FRHITexture* GroundTruthAmbientOcclusionDebugOutput
	) -> FPostProcessPassResult
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
			auto* DebugTargets = ResolvedFrame.Targets.GBufferDebug
				? &*ResolvedFrame.Targets.GBufferDebug : nullptr;
			if (DebugTargets != nullptr
				&& GBufferDebugRenderer.Render_RenderThread(
					CommandList,
					GBufferTargets->Material,
					GBufferTargets->Normals,
					GBufferTargets->Surface,
					GBufferTargets->Emissive,
					SceneTargets.Depth,
					DebugTargets->Color,
					RenderView,
					Options.GBufferDebugMode,
					Width,
					Height
				))
			{
				PostProcessInput = DebugTargets->Color;
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
			return {
				.Result = ERenderViewResult::Success,
				.Input = PostProcessInput};
		}

		FRHIRenderPassInfo EditorAssistancePassInfo{};
		EditorAssistancePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeEditorAssistanceOutput(ViewportOutput);
		EditorAssistancePassInfo.ColorRenderTargets[0] = OutputTarget;
		EditorAssistancePassInfo.DepthStencilRenderTarget =
			SceneTargets.Depth;
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
		return {
			.Result = ERenderViewResult::Success,
			.Input = PostProcessInput,
			.bEditorAssistance = true};
	}

	auto FFixedSceneFrameExecutor::RenderVolumetricCloud_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		FRHITexture* VolumetricCloudShadowVisibility
	) -> FVolumetricCloudPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = PreparedView.Context.View;
		const uint32 Width = SceneColor != nullptr ? SceneColor->GetSizeX() : 0;
		const uint32 Height = SceneColor != nullptr ? SceneColor->GetSizeY() : 0;
		const FPreparedVolumetricCloud* Cloud = PreparedView.VolumetricCloud
			? &*PreparedView.VolumetricCloud : nullptr;
		const FResolvedVolumetricCloud* ResolvedCloud =
			ResolvedFrame.VolumetricCloud
				? &*ResolvedFrame.VolumetricCloud : nullptr;
		const bool bInputsPresent = Cloud != nullptr && ResolvedCloud != nullptr
									&& ResolvedCloud->Textures.BaseDensity != nullptr
									&& ResolvedCloud->Textures.DetailDensity != nullptr
									&& ResolvedCloud->Textures.DensitySampler != nullptr
									&& Depth != nullptr;
		const auto QualityTier = CanonicalizeFixedFrameCloudQuality(
			View.Settings.VolumetricCloud.Quality);
		const auto Quality = FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(
			QualityTier
		);
		const auto CloudExtent = FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
			Width, Height, Quality
		);
		auto* FragmentTargets = bInputsPresent
			&& ResolvedFrame.Targets.VolumetricCloudFragment
			? &*ResolvedFrame.Targets.VolumetricCloudFragment : nullptr;
		auto* ComputeTargets = bInputsPresent
			&& !Qualification.bForceFragmentVolumetricCloud
			&& ResolvedFrame.Targets.VolumetricCloudCompute
			? &*ResolvedFrame.Targets.VolumetricCloudCompute : nullptr;
		auto Textures = ResolvedCloud != nullptr
			? ResolvedCloud->Textures
			: FVolumetricCloudRenderer::FTextureBindings{};
		Textures.SceneDepth = Depth;
		const FVolumetricCloudRenderer::FRenderResult Result =
			VolumetricCloudRenderer.Render_RenderThread(CommandList, FragmentTargets, ComputeTargets, {.bRequested = Cloud != nullptr, .Textures = Textures, .Parameters = Cloud != nullptr ? Cloud->Parameters : FVolumetricCloudRenderer::FParameters{}, .View = &View, .QualityTier = QualityTier, .SuccessfulSequence = TemporalContext.SuccessfulSequence, .Width = CloudExtent.Width, .Height = CloudExtent.Height, .OutputWidth = Width, .OutputHeight = Height});
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
		const FVolumetricCloudRenderer::FTemporalReconstructionResult Temporal =
			Result.Cloud != nullptr ? VolumetricCloudRenderer.ReconstructTemporal_RenderThread(
										  CommandList, {.CurrentCloud = Result.Cloud, .View = &View, .TemporalContext = &TemporalContext, .ViewState = ViewState, .Parameters = Cloud != nullptr ? Cloud->Parameters : FVolumetricCloudRenderer::FParameters{}, .QualityTier = QualityTier, .CloudHistoryKey = Cloud != nullptr ? Cloud->HistoryKey : 0}
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
			&& ResolvedFrame.Targets.VolumetricCloudComposite
			? VolumetricCloudRenderer.Composite_RenderThread(
				CommandList,
				*ResolvedFrame.Targets.VolumetricCloudComposite,
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
				.SceneColor = Composite};
		}
		return {
			.Status = Cloud != nullptr
				? EScenePassStatus::Failed : EScenePassStatus::NotRequested,
			.SceneColor = SceneColor};
	}

	auto FFixedSceneFrameExecutor::RenderScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		const FDeferredDirectionalLightingRenderer::FRenderParameters*
			DeferredParameters,
		FRHITexture* VolumetricCloudShadowVisibility
	) -> FSceneColorPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = PreparedView.Context.View;
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
				CommandList, PreparedView, SceneColor
			);
			CommandList.EndRenderPass();
			return {
				.Result = bRendered ? ERenderViewResult::Success
					: ERenderViewResult::RequiredEnvironmentUnavailable,
				.SceneColor = SceneColor};
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
			return {
				.Result = ERenderViewResult::RequiredEnvironmentUnavailable,
				.SceneColor = SceneColor};

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
			const auto& StaticDraws = Pass == EMeshBasePass::Opaque ? PreparedView.Receiver.StaticMeshes.Opaque : PreparedView.Receiver.StaticMeshes.Masked;
			for (const FPreparedStaticMeshDraw& Draw : StaticDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						PreparedView.Receiver.StaticMeshes,
						ResolvedFrame.Receiver.StaticMeshes, true
					);
				}
			const auto& SkeletalDraws = Pass == EMeshBasePass::Opaque ? PreparedView.Receiver.SkeletalMeshes.Opaque : PreparedView.Receiver.SkeletalMeshes.Masked;
			for (const FPreparedSkeletalMeshDraw& Draw : SkeletalDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Pass, Draw,
						PreparedView.Receiver.SkeletalMeshes,
						ResolvedFrame.Receiver.SkeletalMeshes, true
					);
				}
			const auto& TerrainDraws = Pass == EMeshBasePass::Opaque ? PreparedView.Receiver.Terrains.Opaque : PreparedView.Receiver.Terrains.Masked;
			for (const FPreparedTerrainDraw& Draw : TerrainDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					TerrainRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
						View.Settings.Mode.RenderMode, Draw,
						PreparedView.Receiver.Terrains,
						ResolvedFrame.Receiver.Terrains,
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

		const FVolumetricCloudPassResult CloudResult =
			RenderVolumetricCloud_RenderThread(
				CommandList, PreparedView, SceneColor, Depth,
				VolumetricCloudShadowVisibility);
		SceneColor = CloudResult.SceneColor;
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
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					PreparedView.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.StaticMeshes,
					ResolvedFrame.Receiver.StaticMeshes, true
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					PreparedView.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes, true
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					PreparedView.Receiver.Terrains.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.Terrains,
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
			.SceneColor = SceneColor,
			.VolumetricCloud = CloudResult};
	}

	auto FFixedSceneFrameExecutor::RenderSpecialForwardScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
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
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				PreparedView.Receiver.StaticMeshes,
				ResolvedFrame.Receiver.StaticMeshes
			);
			SkeletalMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				PreparedView.Receiver.SkeletalMeshes,
				ResolvedFrame.Receiver.SkeletalMeshes
			);
			TerrainRenderer.ExecutePass_RenderThread(
				CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
				View.Settings.Mode.RenderMode, Pass,
				PreparedView.Receiver.Terrains,
				ResolvedFrame.Receiver.Terrains
			);
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 PreparedView.Receiver.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					PreparedView.Receiver.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.StaticMeshes,
					ResolvedFrame.Receiver.StaticMeshes
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode, EMeshBasePass::Translucent,
					PreparedView.Receiver.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, ResolvedFrame.Lighting.UniformBuffer,
					View.Settings.Mode.RenderMode,
					PreparedView.Receiver.Terrains.Translucent[Draw.DrawIndex],
					PreparedView.Receiver.Terrains,
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
