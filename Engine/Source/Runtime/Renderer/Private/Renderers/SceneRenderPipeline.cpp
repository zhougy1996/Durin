#include "Renderers/SceneRenderPipeline.h"
#include "Renderers/SceneRenderGraphComposer.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RDG.h"
#include "RenderingThread.h"
#include "Resources/RenderTargetLayouts.h"
#include "Scene.h"
#include "SceneView.h"

#include <limits>

namespace Durin
{
	namespace
	{
		auto ReportSceneRenderGraphRejectedViewState(
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

		class FSceneRenderViewStateSubmission final
		{
		public:
			explicit FSceneRenderViewStateSubmission(FSceneViewState* InState)
				: State(InState)
			{
			}

			~FSceneRenderViewStateSubmission()
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

		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}
	} // namespace

	FSceneRenderPipeline::FSceneRenderPipeline(FSceneRenderer& Renderer)
		: Renderer(Renderer)
	{
	}

	auto FSceneRenderPipeline::Execute_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics,
		FRDGCapture* OutRenderGraphCapture
	) -> ERenderViewResult
	{
		check(IsInRenderingThread());
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderView");
		FSceneFrameContext Context;
		FSceneRenderTelemetry& Telemetry = Context.Observation.Telemetry;
		FResolvedSceneResources& ResolvedSceneResources = Context.Resolved.Scene;
		FSceneViewTemporalContext& TemporalContext = Context.Transaction.Temporal;
		FSceneViewState*& ViewState = Context.Transaction.ViewState;
		Context.Logical.Qualification = GetRendererQualificationPolicy();
		const FRendererQualificationPolicy& Qualification =
			Context.Logical.Qualification;
		auto& DefaultTextures = Renderer.DefaultTextures;
		auto& EnvironmentLighting = Renderer.EnvironmentLighting;
		auto& SkyBoxRenderer = Renderer.SkyBoxRenderer;
		auto& PostProcessRenderer = Renderer.PostProcessRenderer;
		auto& StaticMeshRenderer = Renderer.StaticMeshRenderer;
		auto& SkeletalMeshRenderer = Renderer.SkeletalMeshRenderer;
		auto& TerrainRenderer = Renderer.TerrainRenderer;
		auto& ContactShadowRenderer = Renderer.ContactShadowRenderer;
		auto& VolumetricCloudRenderer = Renderer.VolumetricCloudRenderer;
		auto& VolumetricCloudShadowRenderer = Renderer.VolumetricCloudShadowRenderer;
		auto& EditorAssistanceRenderer = Renderer.EditorAssistanceRenderer;
		if (Renderer.RenderSubmissionSerial != std::numeric_limits<uint64>::max())
			++Renderer.RenderSubmissionSerial;
		Telemetry.View.VolumetricCloud.VolumetricCloudQuality =
			CanonicalizeVolumetricCloudQuality(View.Settings.VolumetricCloud.Quality);
		Telemetry.View.VolumetricCloud.VolumetricCloudDebugMode =
			CanonicalizeVolumetricCloudDebugMode(View.Settings.VolumetricCloud.DebugMode);
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
		Context.Logical.Scene = Scene;
		Context.Logical.CallerView = &View;
		Context.Logical.OutputTarget = OutputTarget;
		Context.Logical.Options = Options;
		Context.Logical.Width = Width;
		Context.Logical.Height = Height;
		Context.Logical.bPresentOutput = bPresentOutput;
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
		Context.Logical.RenderView = FSceneRenderer::FitViewToOutput(
			View, Width, Height);
		FSceneView& RenderView = Context.Logical.RenderView;
		ViewState = Renderer.ViewStates.Find(RenderView.ViewStateId);
		if (ViewState != nullptr && ViewState->IsSubmissionActive())
		{
			TemporalContext.Current =
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				);
			TemporalContext.SubmissionSerial =
				Renderer.RenderSubmissionSerial;
			TemporalContext.Discontinuities =
				ESceneViewDiscontinuity::DuplicateSubmission;
			ReportSceneRenderGraphRejectedViewState(
				"an interleaved submission for",
				RenderView.ViewStateId
			);
			ViewState = nullptr;
		}
		FSceneRenderViewStateSubmission ViewStateSubmission(ViewState);
		if (ViewState != nullptr)
		{
			TemporalContext = ViewState->Begin(
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				),
				Renderer.RenderSubmissionSerial, RenderView.bDiscardHistory
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
				Renderer.RenderSubmissionSerial;
			TemporalContext.Discontinuities =
				ESceneViewDiscontinuity::MissingState;
			if (RenderView.ViewStateId.IsValid())
			{
				ReportSceneRenderGraphRejectedViewState(
					"a missing, released, or foreign",
					RenderView.ViewStateId
				);
			}
		}
		FSceneRenderPreparationResult Preparation = PrepareView_RenderThread(
			CommandList, Context);
		if (!Preparation.IsSuccess()) return Preparation.Result;
		Context.Logical.PreparedView = std::move(*Preparation.Plan);
		const FSceneRenderPlan& PreparedView = *Context.Logical.PreparedView;
		const ERenderViewResult ResolutionResult =
			ResolveSceneRenderResources_RenderThread(
				CommandList, PreparedView, Context);
		if (ResolutionResult != ERenderViewResult::Success)
			return ResolutionResult;
		Context.Features.Plan = BuildSceneFrameFeaturePlan(
			PreparedView, Options, Width, Height, Qualification);
		FSceneFrameFeaturePlan& FeaturePlan = Context.Features.Plan;
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		const RendererEditorAssistance::FRequest EditorAssistanceRequest =
			FEditorAssistanceRenderer::AnalyzeRequest(RenderView, ViewportOutput,
				PreparedView.Context.RendererSimpleElements);
		RendererEditorAssistance::FPrepared& PreparedEditorAssistance =
			Context.Logical.EditorAssistance;
		if (!EditorAssistanceRequest.IsEmpty())
			PreparedEditorAssistance = EditorAssistanceRenderer.Prepare_RenderThread(
				CommandList, RenderView, EditorAssistanceRequest,
				PreparedView.Context.RendererSimpleElements);
		Context.Logical.bHasEditorAssistance =
			PreparedEditorAssistance.HasDrawableOperation();
		if (Context.Logical.bHasEditorAssistance)
			FeaturePlan.EditorAssistance.Purposes =
				ESceneFeaturePurpose::Production;
		const bool bWantsProductionDeferred =
			FeaturePlan.RequiresProductionDeferred();
		Context.Resolved.bHybridRetainedResourcesReady =
			!bWantsProductionDeferred
			|| (StaticMeshRenderer.PrepareHybridRetainedResources_RenderThread(
					PreparedView.Receiver.StaticMeshes,
					ResolvedSceneResources.Receiver.StaticMeshes
				)
				&& SkeletalMeshRenderer.PrepareHybridRetainedResources_RenderThread(
					PreparedView.Receiver.SkeletalMeshes,
					ResolvedSceneResources.Receiver.SkeletalMeshes
				)
				&& TerrainRenderer.PrepareHybridRetainedResources_RenderThread(
					CommandList, PreparedView.Receiver.Terrains,
					ResolvedSceneResources.Receiver.Terrains
				));
		const bool bNeedsGBuffer = FeaturePlan.GBuffer.IsEnabled();
		auto& PreparedContactRoute = FeaturePlan.ContactVisibility.Decision;
		const bool bForceContactShadowVisibilityFragment =
			Qualification.bForceFragmentContactVisibility
			|| RenderView.Settings.DirectionalShadow.ContactRoutePreference
				== EContactShadowRoutePreference::Fragment;
		const bool bForceContactShadowVisibilityCompute =
			!Qualification.bForceFragmentContactVisibility
			&& RenderView.Settings.DirectionalShadow.ContactRoutePreference
				== EContactShadowRoutePreference::Compute;
		if (FeaturePlan.ContactVisibility.IsEnabled()
			&& PreparedView.DirectionalShadow)
		{
			PreparedContactRoute = ContactShadowRenderer.PrepareRoute_RenderThread(
				CommandList, true, bNeedsGBuffer,
				!bForceContactShadowVisibilityCompute,
				!bForceContactShadowVisibilityFragment, RenderView,
				PreparedView.DirectionalShadow->View.LightDirection, Width, Height);
		}
		FRHITexture*& CloudWeatherTexture = Context.Resolved.CloudWeatherTexture;
		const bool bForceCloudFragment =
			Qualification.bForceFragmentVolumetricCloud;
		if (ResolvedSceneResources.VolumetricCloud)
		{
			CloudWeatherTexture = ResolvedSceneResources.VolumetricCloud->Textures.Weather;
			if (!CloudWeatherTexture)
				CloudWeatherTexture = DefaultTextures.Get_RenderThread(
					EDefaultTexture::White);
		}
		if (FeaturePlan.CloudShadow.IsEnabled()
			&& PreparedView.VolumetricCloud && ResolvedSceneResources.VolumetricCloud)
		{
			const auto Prepared =
				VolumetricCloudShadowRenderer.PrepareRoute_RenderThread(CommandList,
				{.bRequested = true,
					.BaseDensity = ResolvedSceneResources.VolumetricCloud->Textures.BaseDensity,
					.DetailDensity = ResolvedSceneResources.VolumetricCloud->Textures.DetailDensity,
					.Weather = CloudWeatherTexture,
					.DensitySampler =
						ResolvedSceneResources.VolumetricCloud->Textures.DensitySampler,
					.Parameters = PreparedView.VolumetricCloud->Parameters,
					.View = &RenderView,
					.QualityTier = CanonicalizeVolumetricCloudQuality(
						RenderView.Settings.VolumetricCloud.Quality),
					.Width = Width, .Height = Height},
				true, !bForceCloudFragment);
			FeaturePlan.CloudShadow.Decision = Prepared;
		}
		if (FeaturePlan.CloudSpatial.IsEnabled()
			&& PreparedView.VolumetricCloud && ResolvedSceneResources.VolumetricCloud)
		{
			auto Textures = ResolvedSceneResources.VolumetricCloud->Textures;
			Textures.Weather = CloudWeatherTexture;
			Textures.SceneDepth = nullptr;
			const auto Prepared = VolumetricCloudRenderer.PrepareRoute_RenderThread(
				CommandList,
				{.bRequested = true,
					.Textures = Textures,
					.Parameters = PreparedView.VolumetricCloud->Parameters,
					.View = &RenderView,
					.QualityTier = CanonicalizeVolumetricCloudQuality(
						RenderView.Settings.VolumetricCloud.Quality),
					.SuccessfulSequence = TemporalContext.SuccessfulSequence,
					.Width = static_cast<uint32>(std::max(
						FeaturePlan.CloudSpatial.Extent.x, 0)),
					.Height = static_cast<uint32>(std::max(
						FeaturePlan.CloudSpatial.Extent.y, 0)),
					.OutputWidth = Width,
					.OutputHeight = Height},
				true, !bForceCloudFragment);
			FeaturePlan.CloudSpatial.Decision = Prepared;
		}
		FRDGBuilder Graph;
		FSceneRenderGraphComposition& Composition =
			Context.Transaction.Composition;
		FSceneRenderGraphComposer::Compose(Graph, Renderer, Context);
		const ESceneRenderGraphExecutionStatus GraphStatus =
			CompileAndExecuteGraph_RenderThread(
				Graph, CommandList, OutRenderGraphCapture,
				Context.Observation);
		if (GraphStatus == ESceneRenderGraphExecutionStatus::CompileFailed)
			return ERenderViewResult::RendererResourcesUnavailable;
		if (GraphStatus == ESceneRenderGraphExecutionStatus::ExecutionFailed)
			return ERenderViewResult::RendererResourcesUnavailable;
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

	auto FSceneRenderPipeline::CompileAndExecuteGraph_RenderThread(
		FRDGBuilder& Graph,
		FRHICommandListImmediate& CommandList,
		FRDGCapture* OutRenderGraphCapture,
		FSceneFrameContext::FObservation& Observation
	) -> ESceneRenderGraphExecutionStatus
	{
		auto CompiledGraph = Graph.Compile();
		if (!CompiledGraph.IsSuccess())
		{
			DURIN_WARN("Scene render graph compilation failed: {}",
				CompiledGraph.Error);
			return ESceneRenderGraphExecutionStatus::CompileFailed;
		}
		const FRDGStatistics Statistics = CompiledGraph.Graph->GetStatistics();
		if (Statistics.IsStructuralRegressionBudgetExceeded()
			&& !Observation.bReportedRegressionOverage)
		{
			const FRDGBudget& Budget = CompiledGraph.Graph->GetBudget();
			DURIN_WARN(
				"Scene render graph regression budget exceeded: passes={}/{} "
				"dependencies={}/{} buffer-transitions={}/{} "
				"texture-transitions={}/{}",
				Statistics.DeclaredPasses, Budget.RegressionMaxPasses,
				Statistics.Dependencies, Budget.RegressionMaxDependencies,
				Statistics.BufferTransitions,
				Budget.RegressionMaxBufferTransitions,
				Statistics.TextureTransitions,
				Budget.RegressionMaxTextureTransitions);
			Observation.bReportedRegressionOverage = true;
		}
		std::string ExecutionError;
		FRDGExecutionContext ExecutionContext{Renderer.RDGAllocator};
		const bool Executed = CompiledGraph.Graph->Execute(
			CommandList, ExecutionContext, &ExecutionError);
		if (!Executed
			&& !std::exchange(Observation.bReportedExecutionFailure, true))
		{
			DURIN_WARN("Scene render graph execution failed: {}",
				ExecutionError.empty() ? "unspecified error" : ExecutionError);
		}
		PublishSceneRenderGraphCapture(
			*CompiledGraph.Graph, OutRenderGraphCapture);
		return Executed ? ESceneRenderGraphExecutionStatus::Executed
			: ESceneRenderGraphExecutionStatus::ExecutionFailed;
	}
} // namespace Durin
