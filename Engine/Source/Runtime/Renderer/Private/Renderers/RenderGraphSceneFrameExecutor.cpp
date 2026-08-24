#include "Renderers/RenderGraphSceneFrameExecutor.h"

#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneRenderPlan.h"
#include "Renderers/SceneRenderTelemetry.h"
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

	FRenderGraphSceneFrameExecutor::FRenderGraphSceneFrameExecutor(FSceneRenderer& Renderer)
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

	auto FRenderGraphSceneFrameExecutor::Execute_RenderThread(
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
		FSceneFrameRequirements Requirements = BuildFrameRequirements(
			PreparedView, Options, Width, Height);
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
		const RendererEditorAssistance::FRequest EditorAssistanceRequest =
			FEditorAssistanceRenderer::AnalyzeRequest(RenderView, ViewportOutput);
		RendererEditorAssistance::FPrepared PreparedEditorAssistance;
		if (!EditorAssistanceRequest.IsEmpty())
			PreparedEditorAssistance = EditorAssistanceRenderer.Prepare_RenderThread(
				CommandList, RenderView, EditorAssistanceRequest);
		const bool bHasEditorAssistance =
			PreparedEditorAssistance.HasDrawableOperation();
		FSceneFrameOutcome Outcome;
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
		if ((Requirements.bContactFragment || Requirements.bContactCompute)
			&& PreparedView.DirectionalShadow)
		{
			const auto Prepared = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, nullptr, nullptr, nullptr, nullptr, nullptr,
				nullptr, nullptr, RenderView,
				PreparedView.DirectionalShadow->View.LightDirection, Width, Height,
				{.bPreparationOnly = true,
					.bInputsExpected = bNeedsGBuffer,
					.bFragmentTargetExpected = Requirements.bContactFragment,
					.bComputeTargetExpected = Requirements.bContactCompute});
			PreparedContactRoute = {
				.Route = Prepared.Route, .Reason = Prepared.Reason};
			Requirements.bContactFragment = Prepared.Route
				== FContactShadowVisibilityRenderer::ERoute::Fragment;
			Requirements.bContactCompute = Prepared.Route
				== FContactShadowVisibilityRenderer::ERoute::Compute;
		}
		FVolumetricCloudShadowRenderer::ERoute PreparedCloudShadowRoute =
			FVolumetricCloudShadowRenderer::ERoute::FactorOne;
		FRHITexture* CloudWeatherTexture = nullptr;
		if (ResolvedFrame.VolumetricCloud)
		{
			CloudWeatherTexture = ResolvedFrame.VolumetricCloud->Textures.Weather;
			if (!CloudWeatherTexture)
				CloudWeatherTexture = DefaultTextures.Get_RenderThread(
					EDefaultTexture::White);
		}
		if ((Requirements.bVolumetricCloudShadowFragment
				|| Requirements.bVolumetricCloudShadowCompute)
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
					.bFragmentTargetExpected =
						Requirements.bVolumetricCloudShadowFragment,
					.bComputeTargetExpected =
						Requirements.bVolumetricCloudShadowCompute});
			PreparedCloudShadowRoute = Prepared.Route;
			Requirements.bVolumetricCloudShadowFragment = Prepared.Route
				== FVolumetricCloudShadowRenderer::ERoute::Fragment;
			Requirements.bVolumetricCloudShadowCompute = Prepared.Route
				== FVolumetricCloudShadowRenderer::ERoute::Compute;
		}
		FVolumetricCloudRenderer::ERoute PreparedCloudRoute =
			FVolumetricCloudRenderer::ERoute::Disabled;
		if ((Requirements.bVolumetricCloudFragment
				|| Requirements.bVolumetricCloudCompute)
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
					.bFragmentTargetExpected =
						Requirements.bVolumetricCloudFragment,
					.bComputeTargetExpected =
						Requirements.bVolumetricCloudCompute});
			PreparedCloudRoute = Prepared.Counters.Route;
			Requirements.bVolumetricCloudFragment = PreparedCloudRoute
				== FVolumetricCloudRenderer::ERoute::Fragment;
			Requirements.bVolumetricCloudCompute = PreparedCloudRoute
				== FVolumetricCloudRenderer::ERoute::Compute;
			Requirements.bVolumetricCloudComposite = PreparedCloudRoute
				!= FVolumetricCloudRenderer::ERoute::Disabled;
		}
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			DeferredParameters;
		std::optional<FDeferredDirectionalLightingRenderer::FRenderParameters>
			ProductionDeferredParameters;
		ERenderViewResult TargetResolutionResult = ERenderViewResult::Success;
		FRenderGraphBuilder Graph;
		constexpr FRenderGraphBudget SceneFrameBudget{
			.MaxPasses = 12,
			.MaxDependencies = 28,
			.MaxTextureTransitions = 20,
			.MaxCompileMicroseconds = 5000,
			.MaxExecuteMicroseconds = 250000,
		};
		Graph.SetBudget(SceneFrameBudget);
		Graph.EnablePassCulling();
		FSceneFrameGraphResources GraphResources;
		std::vector<std::pair<FRHITexture*, FRenderGraphTextureHandle>>
			PersistentTextureImports;
		auto ImportPersistentTexture = [&](std::string_view Name,
			FRHITexture* Texture) -> std::optional<FRenderGraphTextureHandle> {
			if (!Texture) return std::nullopt;
			const auto Existing = std::ranges::find(PersistentTextureImports,
				Texture,
				&std::pair<FRHITexture*, FRenderGraphTextureHandle>::first);
			if (Existing != PersistentTextureImports.end()) return Existing->second;
			const auto Handle = Graph.ImportTexture(Name, Texture,
				ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead);
			PersistentTextureImports.emplace_back(Texture, Handle);
			return Handle;
		};
		FRHITexture* DirectionalShadowTexture =
			DirectionalShadowRenderer.GetTexture_RenderThread();
		if (PreparedView.DirectionalShadow && ResolvedFrame.DirectionalShadow
			&& ResolvedFrame.DirectionalShadow->bEnabled
			&& DirectionalShadowTexture != nullptr)
			GraphResources.DirectionalShadow = ImportPersistentTexture(
				"Scene.DirectionalShadow", DirectionalShadowTexture);
		if (ResolvedFrame.VolumetricCloud)
		{
			GraphResources.VolumetricCloudBaseDensity = ImportPersistentTexture(
				"Scene.VolumetricCloud.BaseDensity",
				ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			GraphResources.VolumetricCloudDetailDensity = ImportPersistentTexture(
				"Scene.VolumetricCloud.DetailDensity",
				ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			GraphResources.VolumetricCloudWeather = ImportPersistentTexture(
				"Scene.VolumetricCloud.Weather", CloudWeatherTexture);
		}
		GraphResources.DefaultWhite = ImportPersistentTexture(
			"Scene.Default.White",
			DefaultTextures.Get_RenderThread(EDefaultTexture::White));
		GraphResources.DefaultShadowArray = ImportPersistentTexture(
			"Scene.Default.ShadowArray", DefaultTextures.GetArray_RenderThread());
		GraphResources.EnvironmentIrradiance = ImportPersistentTexture(
			"Scene.Environment.Irradiance",
			EnvironmentLighting.GetIrradiance_RenderThread());
		GraphResources.EnvironmentPrefiltered = ImportPersistentTexture(
			"Scene.Environment.Prefiltered",
			EnvironmentLighting.GetPrefiltered_RenderThread());
		GraphResources.EnvironmentBrdfLut = ImportPersistentTexture(
			"Scene.Environment.BrdfLut",
			EnvironmentLighting.GetBrdfLut_RenderThread());
		GraphResources.SceneColor = Graph.CreateTexture("Scene.Color",
			FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneColor", Width, Height, EPixelFormat::RGBA16_FLOAT)
				.SetFlags(ETextureCreateFlags::RenderTargetable
					| ETextureCreateFlags::ShaderResource
					| ETextureCreateFlags::SourceCopy),
				.BackingClass = "renderer.scene"}, ERHIAccess::GraphicsShaderRead);
		GraphResources.SceneDepth = Graph.CreateTexture("Scene.Depth",
			FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
				"SceneDepth", Width, Height, EPixelFormat::D32)
				.SetFlags(ETextureCreateFlags::DepthStencilTargetable
					| ETextureCreateFlags::ShaderResource),
				.BackingClass = "renderer.scene"}, ERHIAccess::DepthStencilReadWrite);
		GraphResources.Output = Graph.ImportTexture("Scene.Output", OutputTarget,
			ERHIAccess::Discard,
			bPresentOutput ? ERHIAccess::Present : ERHIAccess::GraphicsShaderRead);
		if (Requirements.bGBuffer)
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
						.BackingClass = "renderer.gbuffer"},
					ERHIAccess::GraphicsShaderRead);
		}
		if (Requirements.bGroundTruthAmbientOcclusion)
		{
			const bool bHalfResolution = Requirements.AmbientOcclusionQuality
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
					Graph.CreateTexture(Names[Index],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[Index],
								NativeWidth, NativeHeight, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								1.0f, 1.0f, 1.0f, 1.0f)),
							.BackingClass = "renderer.ambient-occlusion"},
						ERHIAccess::GraphicsShaderRead);
			if (bHalfResolution)
			{
				GraphResources.GroundTruthAmbientOcclusion[2] =
					Graph.CreateTexture(Names[2],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[2],
								NativeWidth, NativeHeight, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								0.0f, 0.0f, 0.0f, 0.0f)),
							.BackingClass = "renderer.ambient-occlusion"},
						ERHIAccess::GraphicsShaderRead);
				GraphResources.GroundTruthAmbientOcclusion[3] =
					Graph.CreateTexture(Names[3],
						FRenderGraphTextureDesc{.Texture =
							FRHITextureCreateDesc::Create2D(Names[3], Width,
								Height, EPixelFormat::R8_UNORM)
							.SetFlags(ETextureCreateFlags::RenderTargetable
								| ETextureCreateFlags::ShaderResource
								| ETextureCreateFlags::SourceCopy)
							.SetClearValue(FClearValueBinding(
								1.0f, 1.0f, 1.0f, 1.0f)),
							.BackingClass = "renderer.ambient-occlusion"},
						ERHIAccess::GraphicsShaderRead);
			}
		}
		if (Requirements.bContactFragment)
			GraphResources.ContactFragment = Graph.CreateTexture(
				"Scene.ContactVisibility.Fragment",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactVisibility", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy)
					.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f)),
					.BackingClass = "renderer.contact-visibility.fragment"},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.bContactCompute)
			GraphResources.ContactCompute = Graph.CreateTexture(
				"Scene.ContactVisibility.Compute",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DirectionalContactVisibilityCompute", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.BackingClass = "renderer.contact-visibility.compute"},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.bVolumetricCloudShadowFragment)
			GraphResources.VolumetricCloudShadowFragment = Graph.CreateTexture(
				"Scene.VolumetricCloudShadow.Fragment",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudVisibility", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(1.0f, 1.0f, 1.0f, 1.0f)),
					.BackingClass = "renderer.cloud-shadow.fragment"},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.bVolumetricCloudShadowCompute)
			GraphResources.VolumetricCloudShadowCompute = Graph.CreateTexture(
				"Scene.VolumetricCloudShadow.Compute",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudVisibilityCompute", Width, Height,
					EPixelFormat::R8_UNORM)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback),
					.BackingClass = "renderer.cloud-shadow.compute"},
				ERHIAccess::GraphicsShaderRead);
		const uint32 CloudWidth = static_cast<uint32>(
			std::max(Requirements.VolumetricCloudExtent.x, 0));
		const uint32 CloudHeight = static_cast<uint32>(
			std::max(Requirements.VolumetricCloudExtent.y, 0));
		if (Requirements.bVolumetricCloudFragment)
			GraphResources.VolumetricCloudFragment = Graph.CreateTexture(
				"Scene.VolumetricCloud.Fragment",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudFragment", CloudWidth, CloudHeight,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)),
					.BackingClass = "renderer.cloud.fragment"},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.bVolumetricCloudCompute)
			GraphResources.VolumetricCloudCompute = Graph.CreateTexture(
				"Scene.VolumetricCloud.Compute",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudCompute", CloudWidth, CloudHeight,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::Storage
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback),
					.BackingClass = "renderer.cloud.compute"},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.bVolumetricCloudComposite)
			GraphResources.VolumetricCloudComposite = Graph.CreateTexture(
				"Scene.VolumetricCloud.Composite",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"VolumetricCloudComposite", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy
						| ETextureCreateFlags::CPUReadback)
					.SetClearValue(FClearValueBinding(0.0f, 0.0f, 0.0f, 1.0f)),
					.BackingClass = "renderer.cloud.composite"},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.bIsolatedDeferred)
			GraphResources.IsolatedDeferred = Graph.CreateTexture(
				"Scene.Deferred.Isolated",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"DeferredDirectionalColor", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.BackingClass = "renderer.deferred"},
				ERHIAccess::GraphicsShaderRead);
		if (Requirements.bGBufferDebug)
			GraphResources.GBufferDebug = Graph.CreateTexture(
				"Scene.GBuffer.Debug",
				FRenderGraphTextureDesc{.Texture = FRHITextureCreateDesc::Create2D(
					"GBufferDebugColor", Width, Height,
					EPixelFormat::RGBA16_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable
						| ETextureCreateFlags::ShaderResource
						| ETextureCreateFlags::SourceCopy),
					.BackingClass = "renderer.gbuffer-debug"},
				ERHIAccess::GraphicsShaderRead);
		Graph.SetBackingResolver([this, &Requirements, &TargetResolutionResult,
			&GraphResources](auto Requests, auto& Backings,
			std::string& Error) {
			FSceneFrameRequirements RetainedRequirements{
				.Width = Requirements.Width,
				.Height = Requirements.Height,
				.AmbientOcclusionQuality = Requirements.AmbientOcclusionQuality};
			for (const FRenderGraphPreparationRequest& Request : Requests)
			{
				const std::string_view Class = Request.BackingClass;
				if (Class == "renderer.scene") continue;
				if (Class == "renderer.gbuffer")
					RetainedRequirements.bGBuffer = true;
				else if (Class == "renderer.ambient-occlusion")
					RetainedRequirements.bGroundTruthAmbientOcclusion = true;
				else if (Class == "renderer.contact-visibility.fragment")
					RetainedRequirements.bContactFragment = true;
				else if (Class == "renderer.contact-visibility.compute")
					RetainedRequirements.bContactCompute = true;
				else if (Class == "renderer.cloud-shadow.fragment")
					RetainedRequirements.bVolumetricCloudShadowFragment = true;
				else if (Class == "renderer.cloud-shadow.compute")
					RetainedRequirements.bVolumetricCloudShadowCompute = true;
				else if (Class == "renderer.cloud.fragment")
				{
					RetainedRequirements.bVolumetricCloudFragment = true;
					RetainedRequirements.VolumetricCloudExtent = {
						Request.TextureDesc.Extent.x,
						Request.TextureDesc.Extent.y};
				}
				else if (Class == "renderer.cloud.compute")
				{
					RetainedRequirements.bVolumetricCloudCompute = true;
					RetainedRequirements.VolumetricCloudExtent = {
						Request.TextureDesc.Extent.x,
						Request.TextureDesc.Extent.y};
				}
				else if (Class == "renderer.cloud.composite")
					RetainedRequirements.bVolumetricCloudComposite = true;
				else if (Class == "renderer.deferred")
					RetainedRequirements.bIsolatedDeferred = true;
				else if (Class == "renderer.gbuffer-debug")
					RetainedRequirements.bGBufferDebug = true;
				else
				{
					Error = "unknown renderer backing class '" + Request.BackingClass
						+ "'";
					return false;
				}
			}
			TargetResolutionResult = ResolveFrameTargets_RenderThread(
				RetainedRequirements);
			if (TargetResolutionResult != ERenderViewResult::Success)
			{
				Error = "renderer transient target preparation failed";
				return false;
			}
			const auto& Targets = *ResolvedFrame.Targets.Scene;
			auto IsRequested = [&](FRenderGraphTextureHandle Handle) {
				return std::ranges::any_of(Requests,
					[&](const FRenderGraphPreparationRequest& Request) {
						return Request.Kind == ERenderGraphResourceKind::Texture
							&& Request.Texture == Handle;
					});
			};
			bool bComplete = Backings.SetTexture(GraphResources.SceneColor, Targets.Color)
				&& Backings.SetTexture(GraphResources.SceneDepth, Targets.Depth);
			if (GraphResources.GBuffer[0]
				&& IsRequested(*GraphResources.GBuffer[0]))
			{
				if (!ResolvedFrame.Targets.GBuffer) return false;
				const auto& GBuffer = *ResolvedFrame.Targets.GBuffer;
				const std::array Physical{GBuffer.Material, GBuffer.Normals,
					GBuffer.Surface, GBuffer.Emissive};
				for (uint32 Index = 0; Index < GraphResources.GBuffer.size(); ++Index)
					bComplete = Backings.SetTexture(*GraphResources.GBuffer[Index],
						Physical[Index]) && bComplete;
			}
			if (GraphResources.GroundTruthAmbientOcclusion[0]
				&& IsRequested(*GraphResources.GroundTruthAmbientOcclusion[0]))
			{
				if (!ResolvedFrame.Targets.GroundTruthAmbientOcclusion) return false;
				const auto& AmbientOcclusion =
					*ResolvedFrame.Targets.GroundTruthAmbientOcclusion;
				const std::array<FRHITexture*, 4> Physical{
					AmbientOcclusion.Raw, AmbientOcclusion.Scratch,
					AmbientOcclusion.Selector, AmbientOcclusion.Resolved};
				for (uint32 Index = 0;
					Index < GraphResources.GroundTruthAmbientOcclusion.size(); ++Index)
				{
					if (!GraphResources.GroundTruthAmbientOcclusion[Index]) continue;
					bComplete = Backings.SetTexture(
						*GraphResources.GroundTruthAmbientOcclusion[Index],
						Physical[Index]) && bComplete;
				}
			}
			if (GraphResources.ContactFragment
				&& IsRequested(*GraphResources.ContactFragment))
			{
				if (!ResolvedFrame.Targets.ContactFragment) return false;
				bComplete = Backings.SetTexture(*GraphResources.ContactFragment,
					ResolvedFrame.Targets.ContactFragment->Visibility) && bComplete;
			}
			if (GraphResources.ContactCompute
				&& IsRequested(*GraphResources.ContactCompute))
			{
				if (!ResolvedFrame.Targets.ContactCompute) return false;
				bComplete = Backings.SetTexture(*GraphResources.ContactCompute,
					ResolvedFrame.Targets.ContactCompute->Visibility) && bComplete;
			}
			if (GraphResources.VolumetricCloudShadowFragment
				&& IsRequested(*GraphResources.VolumetricCloudShadowFragment))
			{
				if (!ResolvedFrame.Targets.VolumetricCloudShadowFragment) return false;
				bComplete = Backings.SetTexture(
					*GraphResources.VolumetricCloudShadowFragment,
					ResolvedFrame.Targets.VolumetricCloudShadowFragment->Visibility)
					&& bComplete;
			}
			if (GraphResources.VolumetricCloudShadowCompute
				&& IsRequested(*GraphResources.VolumetricCloudShadowCompute))
			{
				if (!ResolvedFrame.Targets.VolumetricCloudShadowCompute) return false;
				bComplete = Backings.SetTexture(
					*GraphResources.VolumetricCloudShadowCompute,
					ResolvedFrame.Targets.VolumetricCloudShadowCompute->Visibility)
					&& bComplete;
			}
			if (GraphResources.VolumetricCloudFragment
				&& IsRequested(*GraphResources.VolumetricCloudFragment))
			{
				if (!ResolvedFrame.Targets.VolumetricCloudFragment) return false;
				bComplete = Backings.SetTexture(
					*GraphResources.VolumetricCloudFragment,
					ResolvedFrame.Targets.VolumetricCloudFragment->Cloud) && bComplete;
			}
			if (GraphResources.VolumetricCloudCompute
				&& IsRequested(*GraphResources.VolumetricCloudCompute))
			{
				if (!ResolvedFrame.Targets.VolumetricCloudCompute) return false;
				bComplete = Backings.SetTexture(
					*GraphResources.VolumetricCloudCompute,
					ResolvedFrame.Targets.VolumetricCloudCompute->Cloud) && bComplete;
			}
			if (GraphResources.VolumetricCloudComposite
				&& IsRequested(*GraphResources.VolumetricCloudComposite))
			{
				if (!ResolvedFrame.Targets.VolumetricCloudComposite) return false;
				bComplete = Backings.SetTexture(
					*GraphResources.VolumetricCloudComposite,
					ResolvedFrame.Targets.VolumetricCloudComposite->Cloud) && bComplete;
			}
			if (GraphResources.IsolatedDeferred
				&& IsRequested(*GraphResources.IsolatedDeferred))
			{
				if (!ResolvedFrame.Targets.IsolatedDeferred) return false;
				bComplete = Backings.SetTexture(*GraphResources.IsolatedDeferred,
					ResolvedFrame.Targets.IsolatedDeferred->Color) && bComplete;
			}
			if (GraphResources.GBufferDebug
				&& IsRequested(*GraphResources.GBufferDebug))
			{
				if (!ResolvedFrame.Targets.GBufferDebug) return false;
				bComplete = Backings.SetTexture(*GraphResources.GBufferDebug,
					ResolvedFrame.Targets.GBufferDebug->Color) && bComplete;
			}
			if (!bComplete) Error = "renderer graph backing publication was incomplete";
			return bComplete;
		});
		const TSceneFrameGraphValue<FDirectionalShadowPassResult> DirectionalShadowValue{
			Graph.CreateToken("Scene.DirectionalShadowValue")};
		const TSceneFrameGraphValue<FGBufferPassResult> GBufferValue{
			Graph.CreateToken("Scene.GBufferValue")};
		const TSceneFrameGraphValue<FGroundTruthAmbientOcclusionPassResult>
			AmbientOcclusionValue{Graph.CreateToken("Scene.AmbientOcclusionValue")};
		const TSceneFrameGraphValue<FContactShadowPassResult> ContactShadowValue{
			Graph.CreateToken("Scene.ContactShadowValue")};
		const TSceneFrameGraphValue<FVolumetricCloudShadowPassResult> CloudShadowValue{
			Graph.CreateToken("Scene.CloudShadowValue")};
		const TSceneFrameGraphValue<FIsolatedDeferredPassResult> DeferredValue{
			Graph.CreateToken("Scene.DeferredValue")};
		const TSceneFrameGraphValue<FSceneColorPassResult> OpaqueSceneValue{
			Graph.CreateToken("Scene.OpaqueValue")};
		const TSceneFrameGraphValue<FVolumetricCloudSpatialPassResult>
			VolumetricCloudSpatialValue{
				Graph.CreateToken("Scene.VolumetricCloudSpatialValue")};
		const TSceneFrameGraphValue<FVolumetricCloudPassResult>
			VolumetricCloudValue{
				Graph.CreateToken("Scene.VolumetricCloudValue")};
		const TSceneFrameGraphValue<FSceneColorPassResult> SceneColorValue{
			Graph.CreateToken("Scene.ColorValue")};
		const TSceneFrameGraphValue<FPostProcessPassResult> PostProcessValue{
			Graph.CreateToken("Scene.PostProcessValue")};
		const TSceneFrameGraphValue<bool> FinalOutputValue{
			Graph.CreateToken("Scene.FinalOutputValue")};
		FSceneColorPassResult OpaqueSceneResult;
		FVolumetricCloudSpatialPassResult VolumetricCloudSpatialResult;
		FVolumetricCloudPassResult VolumetricCloudResult;
		auto DeclarePersistentGraphicsInputs = [&](auto Pass) {
			std::vector<FRenderGraphTextureHandle> Declared;
			auto Declare = [&](const auto& Handle, FRHITexture* Physical) {
				if (!Handle || !Physical
					|| std::ranges::find(Declared, *Handle) != Declared.end())
					return;
				Declared.push_back(*Handle);
				Graph.UseTexture(Pass, *Handle,
					{GetTextureAspects(Physical->GetFormat()), 0,
						Physical->GetNumMips(), 0, Physical->GetArraySize()},
					ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
			};
			Declare(GraphResources.DefaultWhite,
				DefaultTextures.Get_RenderThread(EDefaultTexture::White));
			Declare(GraphResources.DefaultShadowArray,
				DefaultTextures.GetArray_RenderThread());
			Declare(GraphResources.EnvironmentIrradiance,
				EnvironmentLighting.GetIrradiance_RenderThread());
			Declare(GraphResources.EnvironmentPrefiltered,
				EnvironmentLighting.GetPrefiltered_RenderThread());
			Declare(GraphResources.EnvironmentBrdfLut,
				EnvironmentLighting.GetBrdfLut_RenderThread());
		};
		const auto DirectionalShadowPass = Graph.AddPass(
			"Scene.DirectionalShadow", ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &GraphResources](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				Outcome.DirectionalShadow =
					RenderDirectionalShadow_RenderThread(Commands, PreparedView,
						GraphResources.DirectionalShadow
							? Resources.GetTexture(*GraphResources.DirectionalShadow)
							: nullptr);
			});
		Graph.UseToken(DirectionalShadowPass, DirectionalShadowValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.DirectionalShadow)
			Graph.UseManagedDepthStencilAttachment(DirectionalShadowPass,
				*GraphResources.DirectionalShadow,
				{ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
		const auto GBufferPass = Graph.AddPass(
			"Scene.GBuffer", ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &GraphResources, &Options,
				Width, Height, bNeedsGBuffer, bWantsIsolatedDeferred](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GraphResources.GBuffer[0]
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0])
					GBufferTargets = {.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				Outcome.GBuffer = RenderGBuffer_RenderThread(
					Commands, PreparedView, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					Options, Width, Height,
					bNeedsGBuffer, bWantsIsolatedDeferred);
			});
		Graph.UseToken(GBufferPass, GBufferValue.Handle, ERenderGraphUse::Write);
		if (GraphResources.GBuffer[0])
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseManagedColorAttachment(GBufferPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1},
					ERHIRenderTargetLoadAction::Clear,
					ERHIRenderTargetStoreAction::Store,
					ERHIAccess::GraphicsShaderRead);
			Graph.UseManagedDepthStencilAttachment(GBufferPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
		}
		const auto AmbientOcclusionPass = Graph.AddPass(
			"Scene.AmbientOcclusion", ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &GraphResources, &Requirements,
				&Options, Width, Height, bWantsGroundTruthAmbientOcclusion](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0]
					&& Requirements.bGroundTruthAmbientOcclusion)
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
					AmbientOcclusionTargets;
				if (GraphResources.GroundTruthAmbientOcclusion[0])
					AmbientOcclusionTargets = {
						.Raw = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[0]),
						.Scratch = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[1]),
						.Selector = GraphResources.GroundTruthAmbientOcclusion[2]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[2])
							: nullptr,
						.Resolved = GraphResources.GroundTruthAmbientOcclusion[3]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[3])
							: nullptr,
						.Quality = Requirements.AmbientOcclusionQuality};
				Outcome.AmbientOcclusion =
					RenderGroundTruthAmbientOcclusion_RenderThread(
						Commands, PreparedView,
						GBufferTargets ? &*GBufferTargets : nullptr,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						SceneTargets, Options, Width, Height,
						bWantsGroundTruthAmbientOcclusion,
						Outcome.GBuffer.IsComplete());
			});
		Graph.UseToken(AmbientOcclusionPass, GBufferValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(AmbientOcclusionPass, AmbientOcclusionValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.GBuffer[0] && Requirements.bGroundTruthAmbientOcclusion)
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(AmbientOcclusionPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(AmbientOcclusionPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
			for (const auto& Texture :
				GraphResources.GroundTruthAmbientOcclusion)
			{
				if (!Texture) continue;
				Graph.UseManagedTexture(AmbientOcclusionPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1},
					ERenderGraphUse::ReadWrite,
					ERHIAccess::GraphicsShaderRead,
					ERHIAccess::GraphicsShaderRead, true);
			}
		}
		const auto ContactShadowPass = Graph.AddPass(
			"Scene.ContactShadow",
			PreparedContactRoute.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute
				? ERenderGraphPassType::Compute : ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &GraphResources, &Requirements,
				&Options, Width, Height, bWantsProductionDeferred](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0]
					&& (Requirements.bContactFragment
						|| Requirements.bContactCompute))
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FContactShadowVisibilityRenderer::FTargets>
					FragmentContactTargets;
				if (GraphResources.ContactFragment)
					FragmentContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactFragment)};
				std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
					ComputeContactTargets;
				if (GraphResources.ContactCompute)
					ComputeContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactCompute)};
				Outcome.ContactShadow = RenderContactShadows_RenderThread(
					Commands, PreparedView,
					GBufferTargets ? &*GBufferTargets : nullptr,
					FragmentContactTargets ? &*FragmentContactTargets : nullptr,
					ComputeContactTargets ? &*ComputeContactTargets : nullptr,
					SceneTargets, Options, Width, Height,
					bWantsProductionDeferred, Outcome.GBuffer.IsComplete(),
					Outcome.GBuffer.bRenderedGeometry);
			});
		Graph.UseToken(ContactShadowPass, DirectionalShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(ContactShadowPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(ContactShadowPass, ContactShadowValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.GBuffer[0])
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(ContactShadowPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					PreparedContactRoute.Route
							== FContactShadowVisibilityRenderer::ERoute::Compute
						? ERHIAccess::ComputeShaderRead
						: ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(ContactShadowPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				PreparedContactRoute.Route
						== FContactShadowVisibilityRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		}
		if (GraphResources.ContactFragment)
			Graph.UseColorAttachment(ContactShadowPass,
				*GraphResources.ContactFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store);
		if (GraphResources.ContactCompute)
			Graph.UseTexture(ContactShadowPass,
				*GraphResources.ContactCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto CloudShadowPass = Graph.AddPass(
			"Scene.VolumetricCloudShadow",
			PreparedCloudShadowRoute
					== FVolumetricCloudShadowRenderer::ERoute::Compute
				? ERenderGraphPassType::Compute : ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &GraphResources, &Requirements,
				Width, Height, bWantsProductionDeferred](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FVolumetricCloudShadowRenderer::FTargets>
					FragmentTargets;
				if (GraphResources.VolumetricCloudShadowFragment)
					FragmentTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowFragment)};
				std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
					ComputeTargets;
				if (GraphResources.VolumetricCloudShadowCompute)
					ComputeTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowCompute)};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = Requirements.bVolumetricCloudShadowFragment
						|| Requirements.bVolumetricCloudShadowCompute
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				Outcome.VolumetricCloudShadow =
					RenderVolumetricCloudShadows_RenderThread(
						Commands, PreparedView,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						SceneTargets,
						GraphResources.VolumetricCloudBaseDensity
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudBaseDensity) : nullptr,
						GraphResources.VolumetricCloudDetailDensity
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudDetailDensity) : nullptr,
						GraphResources.VolumetricCloudWeather
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudWeather) : nullptr,
						Width, Height, bWantsProductionDeferred,
						Outcome.GBuffer.IsComplete());
			});
		Graph.UseToken(CloudShadowPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(CloudShadowPass, CloudShadowValue.Handle,
			ERenderGraphUse::Write);
		if (Requirements.bVolumetricCloudShadowFragment
			|| Requirements.bVolumetricCloudShadowCompute)
			Graph.UseTexture(CloudShadowPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				PreparedCloudShadowRoute
						== FVolumetricCloudShadowRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		auto DeclareCloudShadowInput = [&](const auto& Texture, FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			Graph.UseTexture(CloudShadowPass, *Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()},
				ERenderGraphUse::Read,
				PreparedCloudShadowRoute
						== FVolumetricCloudShadowRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		};
		if (ResolvedFrame.VolumetricCloud)
		{
			DeclareCloudShadowInput(GraphResources.VolumetricCloudBaseDensity,
				ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			DeclareCloudShadowInput(GraphResources.VolumetricCloudDetailDensity,
				ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			DeclareCloudShadowInput(GraphResources.VolumetricCloudWeather,
				CloudWeatherTexture);
		}
		if (GraphResources.VolumetricCloudShadowFragment)
			Graph.UseManagedTexture(CloudShadowPass,
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead, true);
		if (GraphResources.VolumetricCloudShadowCompute)
			Graph.UseTexture(CloudShadowPass,
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Write, ERHIAccess::ComputeShaderReadWrite, true);
		const auto DeferredPass = Graph.AddPass(
			"Scene.DeferredLighting", ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &GraphResources, &Requirements,
				&Options, &DeferredParameters, &ProductionDeferredParameters,
				Width, Height, bWantsDeferredInputs, bWantsIsolatedDeferred,
				bWantsProductionDeferred, bHybridRetainedResourcesReady](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0])
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = nullptr,
					.Depth = GBufferTargets
						? Resources.GetTexture(GraphResources.SceneDepth) : nullptr};
				std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
					AmbientOcclusionTargets;
				if (GraphResources.GroundTruthAmbientOcclusion[0])
					AmbientOcclusionTargets = {
						.Raw = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[0]),
						.Scratch = Resources.GetTexture(
							*GraphResources.GroundTruthAmbientOcclusion[1]),
						.Selector = GraphResources.GroundTruthAmbientOcclusion[2]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[2])
							: nullptr,
						.Resolved = GraphResources.GroundTruthAmbientOcclusion[3]
							? Resources.GetTexture(
								*GraphResources.GroundTruthAmbientOcclusion[3])
							: nullptr,
						.Quality = Requirements.AmbientOcclusionQuality};
				std::optional<FContactShadowVisibilityRenderer::FTargets>
					FragmentContactTargets;
				if (GraphResources.ContactFragment)
					FragmentContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactFragment)};
				std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
					ComputeContactTargets;
				if (GraphResources.ContactCompute)
					ComputeContactTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.ContactCompute)};
				std::optional<FVolumetricCloudShadowRenderer::FTargets>
					FragmentCloudShadowTargets;
				if (GraphResources.VolumetricCloudShadowFragment)
					FragmentCloudShadowTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowFragment)};
				std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
					ComputeCloudShadowTargets;
				if (GraphResources.VolumetricCloudShadowCompute)
					ComputeCloudShadowTargets = {.Visibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowCompute)};
				DeferredParameters = bWantsDeferredInputs
					? BuildDeferredParameters(
						PreparedView, Outcome.DirectionalShadow,
						GraphResources.DirectionalShadow
							? Resources.GetTexture(*GraphResources.DirectionalShadow)
							: nullptr,
						Outcome.GBuffer,
						GBufferTargets ? &*GBufferTargets : nullptr,
						Outcome.AmbientOcclusion,
						AmbientOcclusionTargets ? &*AmbientOcclusionTargets : nullptr,
						Outcome.ContactShadow,
						FragmentContactTargets ? &*FragmentContactTargets : nullptr,
						ComputeContactTargets ? &*ComputeContactTargets : nullptr,
						Outcome.VolumetricCloudShadow,
						FragmentCloudShadowTargets
							? &*FragmentCloudShadowTargets : nullptr,
						ComputeCloudShadowTargets
							? &*ComputeCloudShadowTargets : nullptr,
						SceneTargets, Options)
					: std::nullopt;
				if (DeferredParameters)
				{
					std::optional<FDeferredDirectionalLightingRenderer::FTargets>
						IsolatedTargets;
					if (GraphResources.IsolatedDeferred)
						IsolatedTargets = {.Color = Resources.GetTexture(
							*GraphResources.IsolatedDeferred)};
					Outcome.IsolatedDeferred = RenderIsolatedDeferred_RenderThread(
						Commands, IsolatedTargets ? &*IsolatedTargets : nullptr,
						*DeferredParameters, Options, Width, Height,
						bWantsIsolatedDeferred);
				}
				else if (bWantsIsolatedDeferred)
				{
					Outcome.IsolatedDeferred.Status = EScenePassStatus::Failed;
					++Telemetry.View.Deferred.DeferredDirectionalUnavailableViews;
				}
				const bool bProductionResourcesReady =
					!bWantsProductionDeferred
					|| (Outcome.GBuffer.IsComplete()
						&& bHybridRetainedResourcesReady
						&& DeferredParameters.has_value());
				if (bWantsProductionDeferred && bProductionResourcesReady)
				{
					ProductionDeferredParameters = *DeferredParameters;
					ProductionDeferredParameters->DiagnosticMode = 0;
				}
			});
		Graph.UseToken(DeferredPass, DirectionalShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, AmbientOcclusionValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, ContactShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, CloudShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(DeferredPass, DeferredValue.Handle, ERenderGraphUse::Write);
		if (GraphResources.DirectionalShadow)
			Graph.UseTexture(DeferredPass, *GraphResources.DirectionalShadow,
				{ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.GBuffer[0])
		{
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(DeferredPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
					ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(DeferredPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		}
		for (const auto& Texture : GraphResources.GroundTruthAmbientOcclusion)
		{
			if (!Texture) continue;
			Graph.UseTexture(DeferredPass, *Texture,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		}
		if (GraphResources.ContactFragment)
			Graph.UseTexture(DeferredPass, *GraphResources.ContactFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.ContactCompute)
			Graph.UseTexture(DeferredPass, *GraphResources.ContactCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudShadowFragment)
			Graph.UseTexture(DeferredPass,
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudShadowCompute)
			Graph.UseTexture(DeferredPass,
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		DeclarePersistentGraphicsInputs(DeferredPass);
		if (GraphResources.IsolatedDeferred)
			Graph.UseManagedColorAttachment(DeferredPass,
				*GraphResources.IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
		const auto OpaqueScenePass = Graph.AddPass(
			"Scene.Opaque", ERenderGraphPassType::Graphics,
			[this, &PreparedView, &GraphResources, &ProductionDeferredParameters,
				&OpaqueSceneResult](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = Resources.GetTexture(GraphResources.SceneColor),
					.Depth = Resources.GetTexture(GraphResources.SceneDepth)};
				const FSceneColorTimingQuerySink TimingSink =
					GetSceneColorTimingQuerySink();
				TScopedRendererGPUTimingQuery Timing(Commands, TimingSink);
				OpaqueSceneResult = RenderSceneOpaque_RenderThread(
					Commands, PreparedView, SceneTargets.Color, SceneTargets.Depth,
					ProductionDeferredParameters
						? &*ProductionDeferredParameters : nullptr);
				Timing.Commit();
			});
		Graph.UseToken(OpaqueScenePass, DeferredValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(OpaqueScenePass, OpaqueSceneValue.Handle,
			ERenderGraphUse::Write);
		if (GraphResources.DirectionalShadow)
			Graph.UseTexture(OpaqueScenePass, *GraphResources.DirectionalShadow,
				{ERHITextureAspect::Depth, 0, 1, 0,
					DirectionalShadowCascadeCount},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		DeclarePersistentGraphicsInputs(OpaqueScenePass);
		Graph.UseManagedColorAttachment(OpaqueScenePass,
			GraphResources.SceneColor,
			{ERHITextureAspect::Color, 0, 1, 0, 1},
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			ERHIAccess::GraphicsShaderRead);
		Graph.UseManagedTexture(OpaqueScenePass, GraphResources.SceneDepth,
			{ERHITextureAspect::Depth, 0, 1, 0, 1},
			ERenderGraphUse::ReadWrite,
			bNeedsGBuffer ? ERHIAccess::GraphicsShaderRead
				: ERHIAccess::DepthStencilReadWrite,
			bRequiresDeferredOpaque ? ERHIAccess::GraphicsShaderRead
				: ERHIAccess::DepthStencilReadWrite,
			!bNeedsGBuffer);

		const auto VolumetricCloudSpatialPass = Graph.AddPass(
			"Scene.VolumetricCloudSpatial",
			PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Compute
				? ERenderGraphPassType::Compute : ERenderGraphPassType::Graphics,
			[this, &PreparedView, &GraphResources, &Requirements,
				&VolumetricCloudSpatialResult](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				std::optional<FVolumetricCloudRenderer::FTargets> FragmentTargets;
				if (GraphResources.VolumetricCloudFragment)
					FragmentTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudFragment)};
				std::optional<FVolumetricCloudRenderer::FComputeTargets> ComputeTargets;
				if (GraphResources.VolumetricCloudCompute)
					ComputeTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudCompute)};
				const FVolumetricCloudTimingQuerySink TimingSink =
					GetVolumetricCloudTimingQuerySink();
				TScopedRendererGPUTimingQuery Timing(Commands, TimingSink);
				VolumetricCloudSpatialResult =
					RenderVolumetricCloudSpatial_RenderThread(
						Commands, PreparedView,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						GraphResources.VolumetricCloudBaseDensity
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudBaseDensity) : nullptr,
						GraphResources.VolumetricCloudDetailDensity
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudDetailDensity) : nullptr,
						GraphResources.VolumetricCloudWeather
							? Resources.GetTexture(
								*GraphResources.VolumetricCloudWeather) : nullptr,
						Requirements.bVolumetricCloudFragment
							|| Requirements.bVolumetricCloudCompute
							? Resources.GetTexture(GraphResources.SceneDepth) : nullptr);
				Timing.Commit();
			});
		Graph.UseToken(VolumetricCloudSpatialPass, OpaqueSceneValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudSpatialPass,
			VolumetricCloudSpatialValue.Handle, ERenderGraphUse::Write);
		auto DeclareCloudSpatialInput = [&](const auto& Texture,
			FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			Graph.UseTexture(VolumetricCloudSpatialPass, *Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()},
				ERenderGraphUse::Read,
				PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		};
		if (ResolvedFrame.VolumetricCloud)
		{
			DeclareCloudSpatialInput(GraphResources.VolumetricCloudBaseDensity,
				ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			DeclareCloudSpatialInput(GraphResources.VolumetricCloudDetailDensity,
				ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			DeclareCloudSpatialInput(GraphResources.VolumetricCloudWeather,
				CloudWeatherTexture);
		}
		if (Requirements.bVolumetricCloudFragment
			|| Requirements.bVolumetricCloudCompute)
			Graph.UseTexture(VolumetricCloudSpatialPass,
				GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				PreparedCloudRoute == FVolumetricCloudRenderer::ERoute::Compute
					? ERHIAccess::ComputeShaderRead
					: ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudFragment)
			Graph.UseManagedTexture(VolumetricCloudSpatialPass,
				*GraphResources.VolumetricCloudFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead, true);
		if (GraphResources.VolumetricCloudCompute)
			Graph.UseTexture(VolumetricCloudSpatialPass,
				*GraphResources.VolumetricCloudCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Write,
				ERHIAccess::ComputeShaderReadWrite, true);

		const auto VolumetricCloudPass = Graph.AddPass(
			"Scene.VolumetricCloud", ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &GraphResources, &Requirements,
				&VolumetricCloudSpatialResult, &VolumetricCloudResult](
				FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				if (!Requirements.bVolumetricCloudComposite) return;
				std::optional<FVolumetricCloudRenderer::FTargets> FragmentTargets;
				if (GraphResources.VolumetricCloudFragment)
					FragmentTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudFragment)};
				std::optional<FVolumetricCloudRenderer::FComputeTargets> ComputeTargets;
				if (GraphResources.VolumetricCloudCompute)
					ComputeTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudCompute)};
				std::optional<FVolumetricCloudRenderer::FTargets> CompositeTargets;
				if (GraphResources.VolumetricCloudComposite)
					CompositeTargets = {.Cloud = Resources.GetTexture(
						*GraphResources.VolumetricCloudComposite)};
				FRHITexture* ShadowVisibility = nullptr;
				if (Outcome.VolumetricCloudShadow.Route
					== EVolumetricCloudShadowPassRoute::Compute
					&& GraphResources.VolumetricCloudShadowCompute)
					ShadowVisibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowCompute);
				else if (Outcome.VolumetricCloudShadow.Route
					== EVolumetricCloudShadowPassRoute::Fragment
					&& GraphResources.VolumetricCloudShadowFragment)
					ShadowVisibility = Resources.GetTexture(
						*GraphResources.VolumetricCloudShadowFragment);
				VolumetricCloudResult =
					RenderVolumetricCloudComposite_RenderThread(
						Commands, PreparedView, VolumetricCloudSpatialResult,
						FragmentTargets ? &*FragmentTargets : nullptr,
						ComputeTargets ? &*ComputeTargets : nullptr,
						CompositeTargets ? &*CompositeTargets : nullptr,
						Resources.GetTexture(GraphResources.SceneColor),
						Resources.GetTexture(GraphResources.SceneDepth),
						ShadowVisibility);
			});
		Graph.UseToken(VolumetricCloudPass, OpaqueSceneValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, VolumetricCloudSpatialValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, CloudShadowValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(VolumetricCloudPass, VolumetricCloudValue.Handle,
			ERenderGraphUse::Write);
		if (Requirements.bVolumetricCloudComposite)
		{
			Graph.UseTexture(VolumetricCloudPass, GraphResources.SceneColor,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
			Graph.UseTexture(VolumetricCloudPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		}
		auto DeclareCloudCompositeInput = [&](const auto& Texture,
			FRHITexture* Physical) {
			if (!Texture || !Physical) return;
			Graph.UseTexture(VolumetricCloudPass, *Texture,
				{GetTextureAspects(Physical->GetFormat()), 0,
					Physical->GetNumMips(), 0, Physical->GetArraySize()},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		};
		if (ResolvedFrame.VolumetricCloud
			&& Requirements.bVolumetricCloudComposite)
		{
			DeclareCloudCompositeInput(GraphResources.VolumetricCloudBaseDensity,
				ResolvedFrame.VolumetricCloud->Textures.BaseDensity);
			DeclareCloudCompositeInput(GraphResources.VolumetricCloudDetailDensity,
				ResolvedFrame.VolumetricCloud->Textures.DetailDensity);
			DeclareCloudCompositeInput(GraphResources.VolumetricCloudWeather,
				CloudWeatherTexture);
		}
		if (GraphResources.VolumetricCloudShadowFragment)
			Graph.UseTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudShadowFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudShadowCompute)
			Graph.UseTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudShadowCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudFragment)
			Graph.UseTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudFragment,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudCompute)
			Graph.UseTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudCompute,
				{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudComposite)
			Graph.UseManagedTexture(VolumetricCloudPass,
				*GraphResources.VolumetricCloudComposite,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::GraphicsShaderRead, true);

		const auto SceneColorPass = Graph.AddPass(
			"Scene.Color", ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &GraphResources, &Requirements,
				&OpaqueSceneResult, &VolumetricCloudResult,
				bRequiresDeferredOpaque](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				if (!bRequiresDeferredOpaque)
					Outcome.SceneColor = OpaqueSceneResult;
				else
				{
					FSceneColorPassResult Input = OpaqueSceneResult;
					if (Requirements.bVolumetricCloudComposite
						&& !VolumetricCloudResult.bCompositeOutputValid)
						Input.Result = ERenderViewResult::RendererResourcesUnavailable;
					FRHITexture* Color = Requirements.bVolumetricCloudComposite
						&& GraphResources.VolumetricCloudComposite
						? Resources.GetTexture(
							*GraphResources.VolumetricCloudComposite)
						: Resources.GetTexture(GraphResources.SceneColor);
					Outcome.SceneColor = RenderSceneTranslucency_RenderThread(
						Commands, PreparedView, Color,
						Resources.GetTexture(GraphResources.SceneDepth), Input,
						VolumetricCloudResult);
				}
				if (!Outcome.SceneColor.IsSuccess()) return;
				ReduceStaticMeshTelemetry(PreparedView.Receiver.StaticMeshes,
					ResolvedFrame.Receiver.StaticMeshes, Telemetry.View);
				ReduceSkeletalMeshTelemetry(PreparedView.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalMeshes,
					ResolvedFrame.Receiver.SkeletalPalettes, Telemetry.View);
				ReduceTerrainTelemetry(PreparedView.Receiver.Terrains,
					ResolvedFrame.Receiver.Terrains, Telemetry.View);
			});
		Graph.UseToken(SceneColorPass, OpaqueSceneValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(SceneColorPass, VolumetricCloudValue.Handle,
			ERenderGraphUse::Read);
		Graph.UseToken(SceneColorPass, SceneColorValue.Handle,
			ERenderGraphUse::Write);
		if (bRequiresDeferredOpaque)
		{
			const FRenderGraphTextureHandle Color =
				Requirements.bVolumetricCloudComposite
					&& GraphResources.VolumetricCloudComposite
				? *GraphResources.VolumetricCloudComposite
				: GraphResources.SceneColor;
			Graph.UseManagedTexture(SceneColorPass, Color,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite,
				ERHIAccess::ColorAttachmentReadWrite,
				ERHIAccess::GraphicsShaderRead);
			Graph.UseManagedTexture(SceneColorPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERenderGraphUse::ReadWrite, ERHIAccess::GraphicsShaderRead,
				ERHIAccess::DepthStencilReadWrite);
		}
		const auto PostProcessPass = Graph.AddPass(
			"Scene.PostProcess", ERenderGraphPassType::Graphics,
			[this, &Outcome, &PreparedView, &View, &GraphResources,
				&Requirements, &Options, bPresentOutput,
				bHasEditorAssistance](FRHICommandListImmediate& Commands,
				const FRenderGraphPassResources& Resources) {
				if (!Outcome.SceneColor.IsSuccess()) return;
				const FPostProcessRenderer::FSceneTargets SceneTargets{
					.Color = Resources.GetTexture(GraphResources.SceneColor),
					.Depth = Requirements.bGBufferDebug
						? Resources.GetTexture(GraphResources.SceneDepth)
						: nullptr};
				FRHITexture* SceneColorInput = SceneTargets.Color;
				if (Outcome.SceneColor.bUsesVolumetricCloudComposite
					&& GraphResources.VolumetricCloudComposite)
					SceneColorInput = Resources.GetTexture(
						*GraphResources.VolumetricCloudComposite);
				std::optional<FGBufferDebugRenderer::FTargets> DebugTargets;
				if (GraphResources.GBufferDebug)
					DebugTargets = {.Color = Resources.GetTexture(
						*GraphResources.GBufferDebug)};
				std::optional<FGBufferRenderer::FTargets> GBufferTargets;
				if (GraphResources.GBuffer[0] && Requirements.bGBufferDebug)
					GBufferTargets = {
						.Material = Resources.GetTexture(*GraphResources.GBuffer[0]),
						.Normals = Resources.GetTexture(*GraphResources.GBuffer[1]),
						.Surface = Resources.GetTexture(*GraphResources.GBuffer[2]),
						.Emissive = Resources.GetTexture(*GraphResources.GBuffer[3])};
				FRHITexture* IsolatedDeferredOutput = nullptr;
				if (GraphResources.IsolatedDeferred
					&& Outcome.IsolatedDeferred.bOutputValid)
					IsolatedDeferredOutput = Resources.GetTexture(
						*GraphResources.IsolatedDeferred);
				Outcome.PostProcess = RenderPostProcess_RenderThread(
					Commands, PreparedView, View, Resources.GetTexture(GraphResources.Output),
					bPresentOutput, Options, SceneTargets,
					GBufferTargets ? &*GBufferTargets : nullptr,
					DebugTargets ? &*DebugTargets : nullptr,
					SceneColorInput, IsolatedDeferredOutput,
					bHasEditorAssistance);
			});
		Graph.UseToken(PostProcessPass, SceneColorValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(PostProcessPass, GBufferValue.Handle, ERenderGraphUse::Read);
		Graph.UseToken(PostProcessPass, PostProcessValue.Handle,
			ERenderGraphUse::Write);
		Graph.UseTexture(PostProcessPass, GraphResources.SceneColor,
			{ERHITextureAspect::Color, 0, 1, 0, 1}, ERenderGraphUse::Read,
			ERHIAccess::GraphicsShaderRead);
		if (GraphResources.VolumetricCloudComposite)
			Graph.UseTexture(PostProcessPass,
				*GraphResources.VolumetricCloudComposite,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (Requirements.bGBufferDebug)
			Graph.UseTexture(PostProcessPass, GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1}, ERenderGraphUse::Read,
				ERHIAccess::GraphicsShaderRead);
		Graph.UseManagedColorAttachment(PostProcessPass, GraphResources.Output,
			{GetTextureAspects(OutputTarget->GetFormat()), 0,
				OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()},
			ERHIRenderTargetLoadAction::Clear,
			ERHIRenderTargetStoreAction::Store,
			bHasEditorAssistance
				? ERHIAccess::ColorAttachmentReadWrite
				: (bPresentOutput ? ERHIAccess::Present
								  : ERHIAccess::GraphicsShaderRead));
		if (GraphResources.GBufferDebug)
			Graph.UseManagedColorAttachment(PostProcessPass,
				*GraphResources.GBufferDebug,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Clear,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::GraphicsShaderRead);
		if (GraphResources.GBuffer[0] && Requirements.bGBufferDebug)
			for (const auto& Texture : GraphResources.GBuffer)
				Graph.UseTexture(PostProcessPass, *Texture,
					{ERHITextureAspect::Color, 0, 1, 0, 1},
					ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (GraphResources.IsolatedDeferred)
			Graph.UseTexture(PostProcessPass, *GraphResources.IsolatedDeferred,
				{ERHITextureAspect::Color, 0, 1, 0, 1},
				ERenderGraphUse::Read, ERHIAccess::GraphicsShaderRead);
		if (!bHasEditorAssistance)
		{
			Graph.UseToken(PostProcessPass, FinalOutputValue.Handle,
				ERenderGraphUse::Write);
			Graph.MarkPassRoot(PostProcessPass,
				bPresentOutput ? "present" : "offscreen-output");
		}
		else
		{
			const auto EditorAssistancePass = Graph.AddPass(
				"Scene.EditorAssistance", ERenderGraphPassType::Graphics,
				[this, &Outcome, &PreparedView, &GraphResources,
					&PreparedEditorAssistance, bPresentOutput](
					FRHICommandListImmediate& Commands,
					const FRenderGraphPassResources& Resources) {
					if (Outcome.PostProcess.Result != ERenderViewResult::Success) return;
					Outcome.PostProcess.bEditorAssistance =
						RenderEditorAssistance_RenderThread(Commands, PreparedView,
							Resources.GetTexture(GraphResources.Output),
							Resources.GetTexture(GraphResources.SceneDepth),
							bPresentOutput, PreparedEditorAssistance);
				});
			Graph.UseToken(EditorAssistancePass, PostProcessValue.Handle,
				ERenderGraphUse::Read);
			Graph.UseToken(EditorAssistancePass, FinalOutputValue.Handle,
				ERenderGraphUse::Write);
			Graph.UseManagedColorAttachment(EditorAssistancePass,
				GraphResources.Output,
				{GetTextureAspects(OutputTarget->GetFormat()), 0,
					OutputTarget->GetNumMips(), 0, OutputTarget->GetArraySize()},
				ERHIRenderTargetLoadAction::Load,
				ERHIRenderTargetStoreAction::Store,
				bPresentOutput ? ERHIAccess::Present
								 : ERHIAccess::GraphicsShaderRead);
			Graph.UseManagedDepthStencilAttachment(EditorAssistancePass,
				GraphResources.SceneDepth,
				{ERHITextureAspect::Depth, 0, 1, 0, 1},
				ERHIRenderTargetLoadAction::Load,
				ERHIRenderTargetStoreAction::Store,
				ERHIAccess::DepthStencilReadWrite);
			Graph.MarkPassRoot(EditorAssistancePass,
				bPresentOutput ? "present" : "offscreen-output");
		}
		auto CompiledGraph = Graph.Compile();
		if (!CompiledGraph.IsSuccess())
		{
			DURIN_WARN("Scene frame graph compilation failed: {}",
				CompiledGraph.Error);
			return ERenderViewResult::RendererResourcesUnavailable;
		}
		std::string ExecutionError;
		const bool bExecuted =
			CompiledGraph.Graph->Execute(CommandList, &ExecutionError);
		PublishSceneRenderGraphCapture(*CompiledGraph.Graph);
		if (!bExecuted)
			return TargetResolutionResult;
		if (!Outcome.SceneColor.IsSuccess()) return Outcome.SceneColor.Result;
		if (Outcome.PostProcess.Result == ERenderViewResult::Success)
		{
			ViewStateSubmission.Commit();
			TelemetryPublication.Commit();
		}
		return Outcome.PostProcess.Result;
	}


	auto FRenderGraphSceneFrameExecutor::RenderGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		const FPostProcessRenderer::FSceneTargets& SceneTargets,
		const FGBufferRenderer::FTargets* GBufferTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bNeedsGBuffer,
		bool bWantsIsolatedDeferred
	) -> FGBufferPassResult
	{
		const FSceneView& RenderView = PreparedView.Context.View;
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

	auto FRenderGraphSceneFrameExecutor::RenderGroundTruthAmbientOcclusion_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
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
		const FSceneView& RenderView = PreparedView.Context.View;
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

	auto FRenderGraphSceneFrameExecutor::RenderContactShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
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
			if (bForceCompute) FragmentContactTargets = nullptr;
			if (bForceFragment) ComputeContactTargets = nullptr;
			Telemetry.View.ContactShadow.ContactShadowRetainedBytes =
				ContactShadowRenderer.GetRetainedTargetBytes_RenderThread();
			const auto ContactResult = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, FragmentContactTargets, ComputeContactTargets,
				GBufferTargets->Material, GBufferTargets->Normals,
				GBufferTargets->Surface, GBufferTargets->Emissive,
				SceneTargets.Depth, RenderView,
				PreparedView.DirectionalShadow->View.LightDirection, Width, Height,
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

	auto FRenderGraphSceneFrameExecutor::RenderVolumetricCloudShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
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
		if (bForceFragment) ComputeTargets = nullptr;
		const auto QualityTier = CanonicalizeRenderGraphFrameCloudQuality(
			PreparedView.Context.View.Settings.VolumetricCloud.Quality);
		const auto Result = VolumetricCloudShadowRenderer.Render_RenderThread(
			CommandList, FragmentTargets, ComputeTargets,
			{.bRequested = true,
				 .BaseDensity = BaseDensity,
				 .DetailDensity = DetailDensity,
			 .Weather = Weather,
			 .SceneDepth = SceneTargets.Depth,
				 .DensitySampler = ResolvedCloud->Textures.DensitySampler,
			 .Parameters = Cloud->Parameters,
			 .View = &PreparedView.Context.View,
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

	auto FRenderGraphSceneFrameExecutor::BuildDeferredParameters(
		const FSceneRenderPlan& PreparedView,
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
			.View = &PreparedView.Context.View,
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

	auto FRenderGraphSceneFrameExecutor::RenderIsolatedDeferred_RenderThread(
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

	auto FRenderGraphSceneFrameExecutor::RenderPostProcess_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
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
		const FSceneView& RenderView = PreparedView.Context.View;
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

	auto FRenderGraphSceneFrameExecutor::RenderEditorAssistance_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		FRHITexture* OutputTarget,
		FRHITexture* DepthTarget,
		bool bPresentOutput,
		const RendererEditorAssistance::FPrepared& Prepared
	) -> bool
	{
		if (!Prepared.HasDrawableOperation()) return false;
		const FSceneView& RenderView = PreparedView.Context.View;
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

	auto FRenderGraphSceneFrameExecutor::RenderVolumetricCloudSpatial_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
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
		const FSceneView& View = PreparedView.Context.View;
		const uint32 Width = Depth != nullptr ? Depth->GetSizeX() : 0;
		const uint32 Height = Depth != nullptr ? Depth->GetSizeY() : 0;
		const FPreparedVolumetricCloud* Cloud = PreparedView.VolumetricCloud
			? &*PreparedView.VolumetricCloud : nullptr;
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

	auto FRenderGraphSceneFrameExecutor::RenderVolumetricCloudComposite_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
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
		const FSceneView& View = PreparedView.Context.View;
		const FPreparedVolumetricCloud* Cloud = PreparedView.VolumetricCloud
			? &*PreparedView.VolumetricCloud : nullptr;
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

	auto FRenderGraphSceneFrameExecutor::RenderSceneOpaque_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		const FDeferredDirectionalLightingRenderer::FRenderParameters*
			DeferredParameters
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
		return {.Result = ERenderViewResult::Success};
	}

	auto FRenderGraphSceneFrameExecutor::RenderSceneTranslucency_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneRenderPlan& PreparedView,
		FRHITexture* SceneColor,
		FRHITexture* Depth,
		const FSceneColorPassResult& Opaque,
		const FVolumetricCloudPassResult& VolumetricCloud
	) -> FSceneColorPassResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		if (!Opaque.IsSuccess()) return Opaque;
		const FSceneView& View = PreparedView.Context.View;
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
			.bUsesVolumetricCloudComposite =
				VolumetricCloud.bCompositeOutputValid,
			.VolumetricCloud = VolumetricCloud};
	}

	auto FRenderGraphSceneFrameExecutor::RenderSpecialForwardScene_RenderThread(
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
