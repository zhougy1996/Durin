#include "Renderers/SceneFrameExecutionPipeline.h"
#include "Renderers/SceneFrameGraphComposer.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Renderers/SceneFrameGraphContributors.h"
#include "Renderers/SceneFrameGraphBackingProvider.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RenderGraph.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "Scene.h"
#include "SceneView.h"

#include <limits>

namespace Durin
{
	namespace
	{
		auto ReportRenderGraphFrameRejectedViewState(
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

		class FRenderGraphFrameViewStateSubmission final
		{
		public:
			explicit FRenderGraphFrameViewStateSubmission(FSceneViewState* InState)
				: State(InState)
			{
			}

			~FRenderGraphFrameViewStateSubmission()
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

		auto CanonicalizeRenderGraphFrameCloudQuality(EVolumetricCloudQuality Quality)
			-> EVolumetricCloudQuality
		{
			return Quality < EVolumetricCloudQuality::Count
				? Quality : EVolumetricCloudQuality::High;
		}

		auto CanonicalizeRenderGraphFrameCloudDebugMode(EVolumetricCloudDebugMode Mode)
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

	FSceneFrameExecutionPipeline::FSceneFrameExecutionPipeline(FSceneRenderer& Renderer)
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
		, Recorders(Renderer, Telemetry, ResolvedFrame, TemporalContext, ViewState)
	{
	}

	auto FSceneFrameExecutionPipeline::Execute_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics,
		const FSceneFrameGraphExecute& ExecuteGraph
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
			CanonicalizeRenderGraphFrameCloudQuality(View.Settings.VolumetricCloud.Quality);
		Telemetry.View.VolumetricCloud.VolumetricCloudDebugMode =
			CanonicalizeRenderGraphFrameCloudDebugMode(View.Settings.VolumetricCloud.DebugMode);
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
			ReportRenderGraphFrameRejectedViewState(
				"an interleaved submission for",
				RenderView.ViewStateId
			);
			ViewState = nullptr;
		}
		FRenderGraphFrameViewStateSubmission ViewStateSubmission(ViewState);
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
				ReportRenderGraphFrameRejectedViewState(
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
		FSceneFrameTopology Requirements = BuildFrameTopology(
			PreparedView, Options, Width, Height);
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		const RendererEditorAssistance::FRequest EditorAssistanceRequest =
			FEditorAssistanceRenderer::AnalyzeRequest(RenderView, ViewportOutput,
				PreparedView.Context.RendererSimpleElements);
		RendererEditorAssistance::FPrepared PreparedEditorAssistance;
		if (!EditorAssistanceRequest.IsEmpty())
			PreparedEditorAssistance = EditorAssistanceRenderer.Prepare_RenderThread(
				CommandList, RenderView, EditorAssistanceRequest,
				PreparedView.Context.RendererSimpleElements);
		const bool bHasEditorAssistance =
			PreparedEditorAssistance.HasDrawableOperation();
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
		FContactShadowVisibilityRenderer::FRouteDecision PreparedContactRoute;
		const bool bForceContactShadowVisibilityFragment =
			Qualification.bForceFragmentContactVisibility
			|| RenderView.Settings.DirectionalShadow.ContactRoutePreference
				== EContactShadowRoutePreference::Fragment;
		const bool bForceContactShadowVisibilityCompute =
			!Qualification.bForceFragmentContactVisibility
			&& RenderView.Settings.DirectionalShadow.ContactRoutePreference
				== EContactShadowRoutePreference::Compute;
		if (Requirements.ContactShadowVisibility != ESceneFrameRoute::Disabled
			&& PreparedView.DirectionalShadow)
		{
			const auto Prepared = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, nullptr, nullptr, nullptr, nullptr, nullptr,
				nullptr, nullptr, RenderView,
				PreparedView.DirectionalShadow->View.LightDirection, Width, Height,
				{.bPreparationOnly = true,
					.bInputsExpected = bNeedsGBuffer,
					.bFragmentTargetExpected = !bForceContactShadowVisibilityCompute,
					.bComputeTargetExpected = !bForceContactShadowVisibilityFragment});
			PreparedContactRoute = {
				.Route = Prepared.Route, .Reason = Prepared.Reason};
			Requirements.ContactShadowVisibility = Prepared.Route
				== FContactShadowVisibilityRenderer::ERoute::Fragment
				? ESceneFrameRoute::Fragment
				: (Prepared.Route == FContactShadowVisibilityRenderer::ERoute::Compute
					? ESceneFrameRoute::Compute : ESceneFrameRoute::Disabled);
		}
		FVolumetricCloudShadowRenderer::ERoute PreparedCloudShadowRoute =
			FVolumetricCloudShadowRenderer::ERoute::FactorOne;
		FRHITexture* CloudWeatherTexture = nullptr;
		const bool bForceCloudFragment =
			Qualification.bForceFragmentVolumetricCloud;
		if (ResolvedFrame.VolumetricCloud)
		{
			CloudWeatherTexture = ResolvedFrame.VolumetricCloud->Textures.Weather;
			if (!CloudWeatherTexture)
				CloudWeatherTexture = DefaultTextures.Get_RenderThread(
					EDefaultTexture::White);
		}
		if (Requirements.VolumetricCloudShadow != ESceneFrameRoute::Disabled
			&& PreparedView.VolumetricCloud && ResolvedFrame.VolumetricCloud)
		{
			const auto Prepared = VolumetricCloudShadowRenderer.Render_RenderThread(
				CommandList, nullptr, nullptr,
				{.bRequested = true,
					.BaseDensity = ResolvedFrame.VolumetricCloud->Textures.BaseDensity,
					.DetailDensity = ResolvedFrame.VolumetricCloud->Textures.DetailDensity,
					.Weather = CloudWeatherTexture,
					.DensitySampler =
						ResolvedFrame.VolumetricCloud->Textures.DensitySampler,
					.Parameters = PreparedView.VolumetricCloud->Parameters,
					.View = &RenderView,
					.QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
						RenderView.Settings.VolumetricCloud.Quality),
					.Width = Width, .Height = Height},
				{.bPreparationOnly = true,
					.bInputsExpected = true,
					.bFragmentTargetExpected = true,
					.bComputeTargetExpected = !bForceCloudFragment});
			PreparedCloudShadowRoute = Prepared.Route;
			Requirements.VolumetricCloudShadow = Prepared.Route
				== FVolumetricCloudShadowRenderer::ERoute::Fragment
				? ESceneFrameRoute::Fragment
				: (Prepared.Route == FVolumetricCloudShadowRenderer::ERoute::Compute
					? ESceneFrameRoute::Compute : ESceneFrameRoute::Disabled);
		}
		FVolumetricCloudRenderer::ERoute PreparedCloudRoute =
			FVolumetricCloudRenderer::ERoute::Disabled;
		if (Requirements.VolumetricCloud != ESceneFrameRoute::Disabled
			&& PreparedView.VolumetricCloud && ResolvedFrame.VolumetricCloud)
		{
			auto Textures = ResolvedFrame.VolumetricCloud->Textures;
			Textures.Weather = CloudWeatherTexture;
			Textures.SceneDepth = nullptr;
			const auto Prepared = VolumetricCloudRenderer.Render_RenderThread(
				CommandList, nullptr, nullptr,
				{.bRequested = true,
					.Textures = Textures,
					.Parameters = PreparedView.VolumetricCloud->Parameters,
					.View = &RenderView,
					.QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
						RenderView.Settings.VolumetricCloud.Quality),
					.SuccessfulSequence = TemporalContext.SuccessfulSequence,
					.Width = static_cast<uint32>(std::max(
						Requirements.VolumetricCloudExtent.x, 0)),
					.Height = static_cast<uint32>(std::max(
						Requirements.VolumetricCloudExtent.y, 0)),
					.OutputWidth = Width,
					.OutputHeight = Height},
				{.bPreparationOnly = true,
					.bInputsExpected = true,
					.bFragmentTargetExpected = true,
					.bComputeTargetExpected = !bForceCloudFragment});
			PreparedCloudRoute = Prepared.Counters.Route;
			Requirements.VolumetricCloud = PreparedCloudRoute
				== FVolumetricCloudRenderer::ERoute::Fragment
				? ESceneFrameRoute::Fragment
				: (PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Compute
					? ESceneFrameRoute::Compute : ESceneFrameRoute::Disabled);
			Requirements.bVolumetricCloudComposite = PreparedCloudRoute
				!= FVolumetricCloudRenderer::ERoute::Disabled;
		}
		FRenderGraphBuilder Graph;
		FSceneFrameGraphComposition Composition;
		FSceneFrameGraphServices GraphServices{
			.Recorders = Recorders,
			.DefaultTextures = DefaultTextures,
			.EnvironmentLighting = EnvironmentLighting,
			.DirectionalShadowRenderer = DirectionalShadowRenderer,
			.ResolvedFrame = ResolvedFrame,
			.Telemetry = Telemetry,
			.ResolveTargets = [this](const FSceneFrameTopology& Topology) {
				return ResolveFrameTargets_RenderThread(Topology);
			}};
		const FSceneFrameGraphComposeInputs ComposeInputs{
			.Services = GraphServices,
			.PreparedView = PreparedView,
			.View = View,
			.OutputTarget = OutputTarget,
			.Options = Options,
			.Topology = Requirements,
			.EditorAssistance = PreparedEditorAssistance,
			.ContactRoute = PreparedContactRoute,
			.CloudShadowRoute = PreparedCloudShadowRoute,
			.CloudRoute = PreparedCloudRoute,
			.CloudWeatherTexture = CloudWeatherTexture,
			.Width = Width,
			.Height = Height,
			.bPresentOutput = bPresentOutput,
			.bHasEditorAssistance = bHasEditorAssistance,
			.bRequiresDeferredOpaque = bRequiresDeferredOpaque,
			.bWantsIsolatedDeferred = bWantsIsolatedDeferred,
			.bWantsGroundTruthAmbientOcclusion =
				bWantsGroundTruthAmbientOcclusion,
			.bWantsDeferredInputs = bWantsDeferredInputs,
			.bWantsProductionDeferred = bWantsProductionDeferred,
			.bHybridRetainedResourcesReady =
				bHybridRetainedResourcesReady,
			.bNeedsGBuffer = bNeedsGBuffer};
		FSceneFrameGraphComposer::Compose(Graph, ComposeInputs, Composition);
		const ESceneFrameGraphExecutionStatus GraphStatus = ExecuteGraph(Graph);
		if (GraphStatus == ESceneFrameGraphExecutionStatus::CompileFailed)
			return ERenderViewResult::RendererResourcesUnavailable;
		if (GraphStatus == ESceneFrameGraphExecutionStatus::ExecutionFailed)
		{
			return Composition.TargetResolutionResult != ERenderViewResult::Success
				? Composition.TargetResolutionResult
				: ERenderViewResult::RendererResourcesUnavailable;
		}
		if (!Composition.SceneColorPublication.IsSuccess())
			return Composition.SceneColorPublication.Result;
		if (Composition.PostProcessPublication.Result
			== ERenderViewResult::Success)
		{
			ViewStateSubmission.Commit();
			TelemetryPublication.Commit();
		}
		return Composition.PostProcessPublication.Result;
	}
} // namespace Durin
