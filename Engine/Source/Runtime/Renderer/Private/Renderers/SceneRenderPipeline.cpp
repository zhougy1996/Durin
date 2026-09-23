#include "Renderers/SceneRenderPipeline.h"
#include "Renderers/SceneRenderingService.h"
#include "Renderers/SceneViewPreparation.h"
#include "Renderers/SceneRenderer.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRenderTelemetry.h"
#include "Profiling/Profiling.h"
#include "RHICommandList.h"
#include "RDG/RDG.h"
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

	FSceneRenderPipeline::FSceneRenderPipeline(FSceneRenderingService& Service)
		: Service(Service)
	{
	}

	auto FSceneRenderPipeline::Execute_RenderThread(
		FRHICommandListImmediate& CommandList, FScene* Scene, const FSceneView& View,
		FRHITexture* OutputTarget, bool bPresentOutput, const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics, FRDGCapture* OutRenderGraphCapture) -> ERenderViewResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderViewSubmission");
		if (Service.RenderSubmissionSerial != std::numeric_limits<uint64>::max())
			++Service.RenderSubmissionSerial;
		// First consumption joins all admitted PSOs from a preparation attempt on
		// the render owner. Replay stays available and no graph exists during waits.
		for (uint32 Attempt = 0; Attempt < 64; ++Attempt)
		{
			FRenderPipelinePreparationBatch Batch;
			const auto Result = ExecutePreparedAttempt_RenderThread(CommandList, Scene, View,
				OutputTarget, bPresentOutput, Options, OutStatistics, OutRenderGraphCapture);
			{
				DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.WaitForPreparedPipelines");
				if (!Batch.Wait()) return Result;
			}
		}
		return ERenderViewResult::RendererResourcesUnavailable;
	}

	auto FSceneRenderPipeline::ExecutePreparedAttempt_RenderThread(
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
		FSceneRenderer Submission(Service);
		FSceneFrameContext& Context = Submission.Context;
		FSceneRenderTelemetry& Telemetry = Context.Observation.Telemetry;
		FSceneViewState*& ViewState = Context.Transaction.ViewState;
		Context.Logical.Qualification = GetRendererQualificationPolicy();
		const FRendererQualificationPolicy& Qualification =
			Context.Logical.Qualification;
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
		const auto ResourceResult = PrepareViewResources_RenderThread(CommandList, Context);
		if (ResourceResult != ERenderViewResult::Success) return ResourceResult;
		Context.Logical.RenderView = FitSceneViewToOutput(
			View, Width, Height);
		SelectViewState_RenderThread(Context);
		FSceneRenderViewStateSubmission ViewStateSubmission(ViewState);
		BeginTemporalState_RenderThread(Context);
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
		Context.Features = BuildSceneFrameFeaturePlan(
			PreparedView, Options, Width, Height, Qualification);
		const auto FeatureResult = PrepareFeatureResources_RenderThread(CommandList, Context);
		if (FeatureResult != ERenderViewResult::Success) return FeatureResult;
		FRDGBuilder Graph;
		FSceneRenderGraphComposition& Composition =
			Context.Transaction.Composition;
		Submission.Render(Graph);
		if (!ExecuteGraph_RenderThread(
			Graph, CommandList, OutRenderGraphCapture))
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

	auto FSceneRenderPipeline::PrepareViewResources_RenderThread(FRHICommandListImmediate& CommandList, FSceneFrameContext& Context) -> ERenderViewResult
	{
		auto* Scene = Context.Logical.Scene;
		const auto& Options = Context.Logical.Options;
		auto& EnvironmentLighting = Service.EnvironmentLighting;
		auto& SkyBoxRenderer = Service.SkyBoxRenderer;
		auto& PostProcessRenderer = Service.PostProcessRenderer;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.EnsureViewResources");
			if (!PostProcessRenderer.EnsureResources_RenderThread(CommandList))
			{
				return ERenderViewResult::RendererResourcesUnavailable;
			}
			// Generate before Scene Color. World-driven scenes already admitted work
			// at frame start; extra views must not move their refresh deadline.
			// Failure is non-fatal: StaticMeshRenderer binds the complete black
			// environment fallback set instead.
			if (Scene && Scene->SkyLighting->WorldUpdateFrame!=GRenderFrameCounterRenderThread)
				Service.UpdateSkyLighting_RenderThread(CommandList, *Scene);
			EnvironmentLighting.SelectScene_RenderThread(Scene);
			// Sky resources include a static index upload, so initialize them before
			// entering the Scene Color render pass.
			const bool bSkyBoxResourcesReady =
				SkyBoxRenderer.EnsureResources_RenderThread();
			if (Options.Environment && !bSkyBoxResourcesReady)
			{
				return ERenderViewResult::RendererResourcesUnavailable;
			}
		}
		return ERenderViewResult::Success;
	}

	auto FSceneRenderPipeline::SelectViewState_RenderThread(FSceneFrameContext& Context) -> void
	{
		auto* Scene = Context.Logical.Scene;
		const auto Width = Context.Logical.Width;
		const auto Height = Context.Logical.Height;
		auto& RenderView = Context.Logical.RenderView;
		auto*& ViewState = Context.Transaction.ViewState;
		auto& TemporalContext = Context.Transaction.Temporal;
		ViewState = Service.ViewStates.Find(RenderView.ViewStateId);
		if (ViewState != nullptr && ViewState->IsSubmissionActive())
		{
			TemporalContext.Current =
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				);
			TemporalContext.SubmissionSerial =
				Service.RenderSubmissionSerial;
			TemporalContext.Discontinuities =
				ESceneViewDiscontinuity::DuplicateSubmission;
			ReportSceneRenderGraphRejectedViewState(
				"an interleaved submission for",
				RenderView.ViewStateId
			);
			ViewState = nullptr;
		}
	}

	auto FSceneRenderPipeline::BeginTemporalState_RenderThread(FSceneFrameContext& Context) -> void
	{
		auto* Scene = Context.Logical.Scene;
		const auto Width = Context.Logical.Width;
		const auto Height = Context.Logical.Height;
		auto& RenderView = Context.Logical.RenderView;
		auto*& ViewState = Context.Transaction.ViewState;
		auto& TemporalContext = Context.Transaction.Temporal;
		if (ViewState != nullptr)
		{
			TemporalContext = ViewState->Begin(
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				),
				Service.RenderSubmissionSerial, RenderView.bDiscardHistory
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
				Service.RenderSubmissionSerial;
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
	}

	auto FSceneRenderPipeline::PrepareFeatureResources_RenderThread(FRHICommandListImmediate& CommandList, FSceneFrameContext& Context) -> ERenderViewResult
	{
		auto* OutputTarget = Context.Logical.OutputTarget;
		const auto Width = Context.Logical.Width;
		const auto Height = Context.Logical.Height;
		const auto& Qualification = Context.Logical.Qualification;
		auto& RenderView = Context.Logical.RenderView;
		auto& TemporalContext = Context.Transaction.Temporal;
		const auto& PreparedView = *Context.Logical.PreparedView;
		auto& FeaturePlan = Context.Features;
		auto& ResolvedSceneResources = Context.Resolved.Scene;
		const bool bPresentOutput = Context.Logical.bPresentOutput;
		auto& DefaultTextures = Service.DefaultTextures;
		auto& StaticMeshRenderer = Service.StaticMeshRenderer;
		auto& ContactShadowRenderer = Service.ContactShadowRenderer;
		auto& VolumetricCloudRenderer = Service.VolumetricCloudRenderer;
		auto& VolumetricCloudShadowRenderer = Service.VolumetricCloudShadowRenderer;
		auto& EditorAssistanceRenderer = Service.EditorAssistanceRenderer;
		{
			DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.PrepareFeatureResources");
			const RenderTargetLayouts::EViewportOutput ViewportOutput =
				GetViewportOutput(bPresentOutput);
			const RendererEditorAssistance::FRequest EditorAssistanceRequest =
				FEditorAssistanceRenderer::AnalyzeRequest(RenderView, ViewportOutput,
					PreparedView.Context.RendererSimpleElements,
					OutputTarget->GetFormat());
			RendererEditorAssistance::FPrepared& PreparedEditorAssistance =
				Context.Logical.EditorAssistance;
			if (!EditorAssistanceRequest.IsEmpty())
				PreparedEditorAssistance = EditorAssistanceRenderer.Prepare_RenderThread(
					CommandList, RenderView, EditorAssistanceRequest,
					PreparedView.Context.RendererSimpleElements);
			const bool bHasEditorAssistance =
				PreparedEditorAssistance.HasDrawableOperation();
			if (bHasEditorAssistance)
				FeaturePlan.EditorAssistance.Purposes =
					ESceneFeaturePurpose::Production;
			const bool bWantsProductionDeferred =
				FeaturePlan.RequiresProductionDeferred();
			Context.Resolved.bHybridRetainedResourcesReady =
				!bWantsProductionDeferred
				|| StaticMeshRenderer.PrepareHybridRetainedResources_RenderThread(
						PreparedView.Receiver.StaticMeshes,
						ResolvedSceneResources.Receiver.StaticMeshes
					);
			if (!Context.Resolved.bHybridRetainedResourcesReady)
				return ERenderViewResult::RendererResourcesUnavailable;
			const bool bNeedsGBuffer = FeaturePlan.GBuffer.IsEnabled();
			if (!StaticMeshRenderer.PrepareUniforms_RenderThread(CommandList, RenderView,
				PreparedView.Receiver.StaticMeshes, ResolvedSceneResources.Receiver.StaticMeshes,
				bWantsProductionDeferred, bNeedsGBuffer)) return ERenderViewResult::RendererResourcesUnavailable;
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
			bool FixedPipelinesReady = true;
			if (FeaturePlan.GBuffer.IsEnabled())
				FixedPipelinesReady = StaticMeshRenderer.PrepareGBufferPipelines_RenderThread(
					Service.GBufferRenderer, PreparedView.Receiver.StaticMeshes,
					ResolvedSceneResources.Receiver.StaticMeshes) && FixedPipelinesReady;
			if (FixedPipelinesReady)
				FixedPipelinesReady = StaticMeshRenderer.PrepareBindings_RenderThread(
					CommandList, &Service.GBufferRenderer, PreparedView.Receiver.StaticMeshes,
					ResolvedSceneResources.Receiver.StaticMeshes, ResolvedSceneResources.Lighting.UniformBuffer);
			if (FeaturePlan.Deferred.IsEnabled())
				FixedPipelinesReady = Service.DeferredDirectionalLightingRenderer.EnsureResources_RenderThread(CommandList) && FixedPipelinesReady;
			if (FeaturePlan.AmbientOcclusion.IsEnabled())
				FixedPipelinesReady = Service.GroundTruthAmbientOcclusionRenderer.EnsureResources_RenderThread(CommandList) && FixedPipelinesReady;
			if (FeaturePlan.GBufferDebug.IsEnabled())
				FixedPipelinesReady = Service.GBufferDebugRenderer.EnsureResources_RenderThread(CommandList) && FixedPipelinesReady;
			if (FeaturePlan.CloudSpatial.IsEnabled() && PreparedView.VolumetricCloud)
			{
				FixedPipelinesReady = VolumetricCloudRenderer.EnsureCompositeResources_RenderThread(CommandList) && FixedPipelinesReady;
				if (!FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(RenderView.Settings.VolumetricCloud.Quality).IsFullResolution())
					FixedPipelinesReady = VolumetricCloudRenderer.EnsureTemporalResources_RenderThread(CommandList) && FixedPipelinesReady;
			}
			if (!FixedPipelinesReady || FRenderPipelinePreparationBatch::HasPending())
				return ERenderViewResult::RendererResourcesUnavailable;
		}
		return ERenderViewResult::Success;
	}

	auto FSceneRenderPipeline::ExecuteGraph_RenderThread(
		FRDGBuilder& Graph,
		FRHICommandListImmediate& CommandList,
		FRDGCapture* OutRenderGraphCapture
	) -> bool
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.ExecuteGraph");
		FGPUTimingQueryRHIRef Timing;
		if (Service.ViewGPUTimingSink) Timing=GDynamicRHI->RHICreateGPUTimingQuery();
		if (Timing) CommandList.BeginGPUTimingQuery(Timing);
		const auto Result = Graph.Execute(CommandList, &Service.RDGAllocator);
		if (Timing)
		{
			CommandList.EndGPUTimingQuery(Timing);
			Service.ViewGPUTimingSink(std::move(Timing));
		}
		const FRDGStatistics Statistics = Graph.GetStatistics();
		if (Service.RenderGraphWarnings.ShouldReport(Statistics, Graph.GetBudget()))
		{
			const FRDGBudget& Budget = Graph.GetBudget();
			DURIN_WARN(
				"Scene render graph regression budget exceeded: passes={}/{} "
				"dependencies={}/{} buffer-transitions={}/{} "
				"texture-transitions={}/{} texture-subresource-transitions={}",
				Statistics.DeclaredPasses, Budget.RegressionMaxPasses,
				Statistics.Dependencies, Budget.RegressionMaxDependencies,
				Statistics.BufferTransitions,
				Budget.RegressionMaxBufferTransitions,
				Statistics.TextureTransitions,
				Budget.RegressionMaxTextureTransitions, Statistics.TextureTransitionSubresources);
			if (Service.RenderGraphWarnings.IsFull())
				DURIN_WARN("Scene render graph regression warning limit reached; "
					"further warnings are suppressed for this renderer. "
					"Render graph captures still contain complete statistics.");
		}
		const bool Executed = Result.has_value();
		if (!Executed)
		{
			DURIN_WARN("Scene render graph {} failed: {}",
				Durin::GetRDGExecutionStatus(Result) == ERDGExecutionStatus::CompileFailed ? "compilation" : "execution",
				ToString(Result.error()));
		}
		PublishSceneRenderGraphCapture(
			Graph, OutRenderGraphCapture);
		return Executed;
	}
} // namespace Durin
