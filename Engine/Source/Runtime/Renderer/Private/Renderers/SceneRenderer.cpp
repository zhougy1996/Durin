#include "Renderers/SceneRenderer.h"

#include "Engine/TerrainSceneProxy.h"
#include "Renderers/PreparedSceneView.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/TerrainRenderPreparation.h"
#include "Renderers/SceneRendererProfiling.h"

#include "Profiling/Profiling.h"

#include "AssetSystem.h"
#include "Console/ConsoleCommand.h"
#include "EnvironmentLighting/EnvironmentLighting.h"
#include "IScene.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	namespace
	{
		std::atomic<FSceneColorTimingQuerySink> GSceneColorTimingQuerySink = nullptr;

		auto AddSaturated(uint64 A, uint64 B) -> uint64
		{
			return B > std::numeric_limits<uint64>::max() - A
				? std::numeric_limits<uint64>::max()
				: A + B;
		}

		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent
				? RenderTargetLayouts::EViewportOutput::Present
				: RenderTargetLayouts::EViewportOutput::Offscreen;
		}

		auto CopyStaticMeshCounters(
			const FPreparedStaticMeshView& StaticMeshes,
			FViewRenderCounters& Counters) -> void
		{
			Counters.VisibleStaticMeshCandidates = StaticMeshes.VisibleLocalCandidates;
			Counters.PreparedStaticMeshPrimitives = StaticMeshes.PreparedLocalPrimitives;
			Counters.RejectedStaticMeshPrimitives = StaticMeshes.RejectedPrimitives
				- std::min(StaticMeshes.RejectedPrimitives, StaticMeshes.RejectedSplinePrimitives);
			Counters.VisibleSplineMeshCandidates = StaticMeshes.VisibleSplineCandidates;
			Counters.PreparedSplineMeshPrimitives = StaticMeshes.PreparedSplinePrimitives;
			Counters.RejectedSplineMeshPrimitives = StaticMeshes.RejectedSplinePrimitives;
			Counters.PreparedSplineMeshSections = StaticMeshes.PreparedSplineSections;
			Counters.PreparedSplineMeshTriangles = StaticMeshes.PreparedSplineTriangles;
			Counters.RetainedSplineMeshDeformationBytes = StaticMeshes.RetainedSplineDeformationBytes;
			Counters.AcceptedSplineMeshDynamicUpdates = StaticMeshes.AcceptedSplineDynamicUpdates;
			Counters.PreparedStaticMeshSections = StaticMeshes.SelectedSections;
			Counters.PreparedStaticMeshTriangles = StaticMeshes.SelectedTriangles;
			Counters.StaticMeshProjectedSizeFallbacks =
				StaticMeshes.ProjectedSizeFallbacks;
			Counters.StaticMeshResourceFallbacks = StaticMeshes.ResourceFallbacks;
			Counters.RequestedStaticMeshLODHistogram =
				StaticMeshes.RequestedLODHistogram;
			Counters.SelectedStaticMeshLODHistogram =
				StaticMeshes.SelectedLODHistogram;
			Counters.OpaqueStaticMeshSections = StaticMeshes.OpaqueSections;
			Counters.MaskedStaticMeshSections = StaticMeshes.MaskedSections;
			Counters.TranslucentStaticMeshSections =
				StaticMeshes.TranslucentSections;
			Counters.OpaqueStaticMeshTriangles = StaticMeshes.OpaqueTriangles;
			Counters.MaskedStaticMeshTriangles = StaticMeshes.MaskedTriangles;
			Counters.TranslucentStaticMeshTriangles =
				StaticMeshes.TranslucentTriangles;
			Counters.OpaqueStaticMeshStateGroups = StaticMeshes.OpaqueStateGroups;
			Counters.MaskedStaticMeshStateGroups = StaticMeshes.MaskedStateGroups;
			Counters.OpaqueStaticMeshInputStateGroups =
				StaticMeshes.OpaqueInputStateGroups;
			Counters.MaskedStaticMeshInputStateGroups =
				StaticMeshes.MaskedInputStateGroups;
			Counters.StaticMeshPipelineTransitions =
				StaticMeshes.PipelineTransitions;
			Counters.StaticMeshMaterialTransitions =
				StaticMeshes.MaterialTransitions;
			Counters.StaticMeshVertexFactoryTransitions =
				StaticMeshes.VertexFactoryTransitions;
			Counters.StaticMeshGeometryTransitions =
				StaticMeshes.GeometryTransitions;
			Counters.StaticMeshResourceAttemptedDraws =
				StaticMeshes.ResourcePreparationAttemptedDraws;
			Counters.StaticMeshResourceSuccessfulDraws =
				StaticMeshes.ResourcePreparationSuccessfulDraws;
			Counters.StaticMeshResourceRejectedDraws =
				StaticMeshes.ResourcePreparationRejectedDraws;
			Counters.StaticMeshAttemptedDraws = StaticMeshes.AttemptedDraws;
			Counters.StaticMeshSuccessfulDraws = StaticMeshes.SuccessfulDraws;
			Counters.StaticMeshRejectedDraws = StaticMeshes.RejectedDraws;
		}

		auto CopySkeletalMeshCounters(
			const FPreparedSkeletalMeshView& Meshes,
			FViewRenderCounters& Counters) -> void
		{
			Counters.PreparedSkeletalMeshPrimitives = Meshes.Primitives.size();
			Counters.RejectedSkeletalMeshPrimitives = Meshes.RejectedPrimitives;
			Counters.PreparedSkeletalMeshSections = Meshes.SelectedSections;
			Counters.PreparedSkeletalMeshTriangles = Meshes.SelectedTriangles;
			Counters.OpaqueSkeletalMeshSections = Meshes.OpaqueSections;
			Counters.MaskedSkeletalMeshSections = Meshes.MaskedSections;
			Counters.TranslucentSkeletalMeshSections = Meshes.TranslucentSections;
			Counters.OpaqueSkeletalMeshTriangles = Meshes.OpaqueTriangles;
			Counters.MaskedSkeletalMeshTriangles = Meshes.MaskedTriangles;
			Counters.TranslucentSkeletalMeshTriangles = Meshes.TranslucentTriangles;
			Counters.OpaqueSkeletalMeshStateGroups = Meshes.OpaqueStateGroups;
			Counters.MaskedSkeletalMeshStateGroups = Meshes.MaskedStateGroups;
			Counters.SkeletalMeshPipelineTransitions = Meshes.PipelineTransitions;
			Counters.SkeletalMeshMaterialTransitions = Meshes.MaterialTransitions;
			Counters.SkeletalMeshVertexFactoryTransitions =
				Meshes.VertexFactoryTransitions;
			Counters.SkeletalMeshGeometryTransitions = Meshes.GeometryTransitions;
			Counters.SkeletalMeshResourceAttemptedDraws =
				Meshes.ResourcePreparationAttemptedDraws;
			Counters.SkeletalMeshResourceSuccessfulDraws =
				Meshes.ResourcePreparationSuccessfulDraws;
			Counters.SkeletalMeshResourceRejectedDraws =
				Meshes.ResourcePreparationRejectedDraws;
			Counters.SkeletalMeshAttemptedDraws = Meshes.AttemptedDraws;
			Counters.SkeletalMeshSuccessfulDraws = Meshes.SuccessfulDraws;
			Counters.SkeletalMeshRejectedDraws = Meshes.RejectedDraws;
			Counters.RequestedSkeletalPaletteUploads = Meshes.RequestedPaletteUploads;
			Counters.UploadedSkeletalPalettes = Meshes.UploadedPalettes;
			Counters.ReusedSkeletalPalettes = Meshes.ReusedPalettes;
			Counters.RejectedSkeletalPalettes = Meshes.RejectedPalettes;
			Counters.UploadedSkeletalPaletteMatrices = Meshes.UploadedPaletteMatrices;
			Counters.UploadedSkeletalPaletteBytes = Meshes.UploadedPaletteBytes;
		}

		auto CopyTerrainCounters(
			const FPreparedTerrainView& Terrain, FViewRenderCounters& Counters) -> void
		{
			Counters.TerrainPatchCandidates = Terrain.PatchCandidates;
			Counters.VisibleTerrainPatches = Terrain.VisiblePatches;
			Counters.CulledTerrainPatches = Terrain.CulledPatches;
			Counters.InvalidTerrainPatchBounds = Terrain.InvalidBoundsFallbacks;
			Counters.TerrainLODFallbacks = Terrain.LODFallbacks;
			Counters.TerrainLODResolutionFallbacks = Terrain.LODResolutionFallbacks;
			Counters.TerrainAdjacencyPromotions = Terrain.AdjacencyPromotions;
			Counters.TerrainAdjacencyIterations = Terrain.AdjacencyIterations;
			Counters.RequestedTerrainLODHistogram = Terrain.RequestedLODHistogram;
			Counters.ResolvedTerrainLODHistogram = Terrain.ResolvedLODHistogram;
			Counters.TerrainStitchMaskHistogram = Terrain.StitchMaskHistogram;
			Counters.PreparedTerrainTriangles = Terrain.Triangles;
			Counters.OpaqueTerrainPatches = Terrain.Opaque.size();
			Counters.MaskedTerrainPatches = Terrain.Masked.size();
			Counters.TranslucentTerrainPatches = Terrain.Translucent.size();
			Counters.TerrainResourceAttemptedDraws = Terrain.ResourceAttemptedDraws;
			Counters.TerrainResourceSuccessfulDraws = Terrain.ResourceSuccessfulDraws;
			Counters.TerrainResourceRejectedDraws = Terrain.ResourceRejectedDraws;
			Counters.PreparedTerrainBatches = Terrain.PreparedBatches;
			Counters.TerrainBatchChunks = Terrain.BatchChunks;
			Counters.TerrainInstances = Terrain.InstanceCount;
			Counters.TerrainInstanceBytes = Terrain.InstanceBytes;
			Counters.TerrainInstanceAllocations = Terrain.InstanceAllocations;
			Counters.TerrainResourceAttemptedBatches = Terrain.ResourceAttemptedBatches;
			Counters.TerrainResourceSuccessfulBatches = Terrain.ResourceSuccessfulBatches;
			Counters.TerrainResourceRejectedBatches = Terrain.ResourceRejectedBatches;
			Counters.TerrainSubmittedLogicalPatches = Terrain.SubmittedLogicalPatches;
			Counters.TerrainScalarTranslucentDraws = Terrain.ScalarTranslucentDraws;
			Counters.TerrainLogicalPreparationNanoseconds = Terrain.LogicalPreparationNanoseconds;
			Counters.TerrainBatchConstructionNanoseconds = Terrain.BatchConstructionNanoseconds;
			Counters.TerrainResourcePreparationNanoseconds = Terrain.ResourcePreparationNanoseconds;
			Counters.TerrainHeightPreparationNanoseconds = Terrain.HeightPreparationNanoseconds;
			Counters.TerrainTopologyPreparationNanoseconds = Terrain.TopologyPreparationNanoseconds;
			Counters.TerrainShaderPreparationNanoseconds = Terrain.ShaderPreparationNanoseconds;
			Counters.TerrainPipelinePreparationNanoseconds = Terrain.PipelinePreparationNanoseconds;
			Counters.TerrainDynamicAllocationNanoseconds = Terrain.DynamicAllocationNanoseconds;
			Counters.TerrainCommandRecordingNanoseconds = Terrain.CommandRecordingNanoseconds;
			Counters.TerrainAttemptedDraws = Terrain.AttemptedDraws;
			Counters.TerrainSuccessfulDraws = Terrain.SuccessfulDraws;
			Counters.TerrainRejectedDraws = Terrain.RejectedDraws;
			Counters.TerrainHeightUploadBytes = Terrain.HeightUploadBytes;
			Counters.TerrainHeightUploads = Terrain.HeightUploads;
			Counters.TerrainHeightReuses = Terrain.HeightReuses;
			Counters.TerrainTopologyCreations = Terrain.TopologyCreations;
			Counters.TerrainTopologyReuses = Terrain.TopologyReuses;
			Counters.TerrainTopologyBytes = Terrain.TopologyBytes;
			Counters.TerrainShaderLookups = Terrain.ShaderLookups;
			Counters.TerrainShaderCreations = Terrain.ShaderCreations;
			Counters.TerrainShaderReuses = Terrain.ShaderReuses;
			Counters.TerrainPipelineLookups = Terrain.PipelineLookups;
			Counters.TerrainPipelineCreations = Terrain.PipelineCreations;
			Counters.TerrainPipelineReuses = Terrain.PipelineReuses;
		}
	} // namespace

	auto BuildSceneViewStatistics(const FViewRenderCounters& Counters)
		-> FSceneViewStatistics
	{
		FSceneViewStatistics Result;
		Result.SubmittedPrimitives = Counters.SubmittedPrimitives;
		Result.VisiblePrimitives = Counters.VisiblePrimitives;
		Result.StaticMeshPrimitives = Counters.PreparedStaticMeshPrimitives;
		Result.SplineMeshPrimitives = Counters.PreparedSplineMeshPrimitives;
		Result.SkeletalMeshPrimitives = Counters.PreparedSkeletalMeshPrimitives;
		Result.VisibleTerrainPatches = Counters.VisibleTerrainPatches;

		Result.SplineMeshTriangles = Counters.PreparedSplineMeshTriangles;
		Result.StaticMeshTriangles = Counters.PreparedStaticMeshTriangles
			- std::min(Counters.PreparedStaticMeshTriangles,
				Counters.PreparedSplineMeshTriangles);
		Result.SkeletalMeshTriangles = Counters.PreparedSkeletalMeshTriangles;
		Result.TerrainTriangles = Counters.PreparedTerrainTriangles;
		Result.Triangles = AddSaturated(
			AddSaturated(Result.StaticMeshTriangles, Result.SplineMeshTriangles),
			AddSaturated(Result.SkeletalMeshTriangles, Result.TerrainTriangles));
		Result.ShadowTriangles = Counters.ShadowPreparedTriangles;

		Result.StaticMeshDrawCalls = Counters.StaticMeshSuccessfulDraws;
		Result.SkeletalMeshDrawCalls = Counters.SkeletalMeshSuccessfulDraws;
		Result.TerrainDrawCalls = Counters.TerrainSuccessfulDraws;
		Result.ShadowDrawCalls = Counters.ShadowSuccessfulDraws;
		Result.DirectionalLights = Counters.SelectedDirectionalLights;
		Result.PointLights = Counters.SelectedPointLights;
		Result.SpotLights = Counters.SelectedSpotLights;
		Result.ShadowCascades = static_cast<uint32>(std::min<size_t>(
			Counters.ShadowCascadeCount, std::numeric_limits<uint32>::max()));
		Result.bShadowEnabled = Counters.ShadowValidReceiverViews != 0
			&& Counters.ShadowCascadeCount != 0;
		return Result;
	}

	auto SetSceneColorTimingQuerySink(FSceneColorTimingQuerySink Sink) -> void
	{
		GSceneColorTimingQuerySink.store(Sink, std::memory_order_release);
	}

	FSceneRenderer::FSceneRenderer()
		: DefaultTextures(Coordinator)
		, EnvironmentLighting(Coordinator)
		, DirectionalShadowRenderer(Coordinator)
		, StaticMeshRenderer(Coordinator, DefaultTextures, EnvironmentLighting)
		, TerrainRenderer(Coordinator, DefaultTextures, EnvironmentLighting)
		, SkeletalMeshRenderer(Coordinator, DefaultTextures, EnvironmentLighting)
		, SkyBoxRenderer(Coordinator, DefaultTextures)
		, PostProcessRenderer(Coordinator, FullscreenGeometry)
		, EditorAssistanceRenderer(Coordinator, FullscreenGeometry)
	{
	}

	FSceneRenderer::~FSceneRenderer() = default;

	auto FSceneRenderer::Start(FConsoleCommandRegistry& Registry) -> bool
	{
		FAssetPath EnvironmentPath;
		DEnvironmentLighting* EnvironmentAsset = nullptr;
		std::string PathError;
		Asset::FAssetResult EnvironmentResult =
			FAssetPath::TryCreate(
				"/Engine/Renderer/DefaultStudioEnvironment",
				EnvironmentPath,
				&PathError)
			? Asset::LoadAsset(EnvironmentPath, EnvironmentAsset)
			: Asset::FAssetResult{Asset::EAssetError::InvalidPath, std::move(PathError)};
		if (EnvironmentResult && EnvironmentAsset != nullptr)
		{
			EnvironmentLighting.Initialize(EnvironmentAsset->GetData());
		}
		else
		{
			DURIN_ERROR(
				"Failed to load the built-in studio environment: {}",
				EnvironmentResult.Message);
		}
		return Coordinator.Start(
			Registry,
			[this](ERendererResourceInvalidationCause Cause) {
				EnqueueResourceInvalidation(Cause);
			});
	}

	auto FSceneRenderer::Stop() -> void
	{
		Coordinator.Stop();
	}

	auto FSceneRenderer::InitializeStartupResources_RenderThread(
		FRHICommandListImmediate& CommandList) -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.Initialize_RenderThread(CommandList);
	}

	auto FSceneRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		DefaultTextures.ReleaseResources_RenderThread();
		EnvironmentLighting.ReleaseResources_RenderThread();
		StaticMeshRenderer.ReleaseResources_RenderThread();
		TerrainRenderer.ReleaseResources_RenderThread();
		SkeletalMeshRenderer.ReleaseResources_RenderThread();
		DirectionalShadowRenderer.ReleaseResources_RenderThread();
		Coordinator.ReleaseResources_RenderThread();
		SkyBoxRenderer.ReleaseResources_RenderThread();
		EditorAssistanceRenderer.ReleaseResources_RenderThread();
		PostProcessRenderer.ReleaseResources_RenderThread();
		FullscreenGeometry.ReleaseResources_RenderThread();
	}

	auto FSceneRenderer::FitViewToOutput(
		const FSceneView& View,
		uint32 Width,
		uint32 Height) -> FSceneView
	{
		FSceneView RenderView = View;
		RenderView.ViewportX = 0;
		RenderView.ViewportY = 0;
		RenderView.ViewportWidth = Width;
		RenderView.ViewportHeight = Height;
		if (RenderView.AspectRatioConstraint <= 0.0f)
		{
			return RenderView;
		}

		uint32 ContentWidth = Width;
		uint32 ContentHeight = static_cast<uint32>(
			std::round(ContentWidth / RenderView.AspectRatioConstraint));
		if (ContentHeight > Height)
		{
			ContentHeight = Height;
			ContentWidth = static_cast<uint32>(
				std::round(
					ContentHeight * RenderView.AspectRatioConstraint));
		}
		RenderView.ViewportWidth = std::max(1u, ContentWidth);
		RenderView.ViewportHeight = std::max(1u, ContentHeight);
		RenderView.ViewportX = (Width - RenderView.ViewportWidth) / 2;
		RenderView.ViewportY = (Height - RenderView.ViewportHeight) / 2;
		return RenderView;
	}

	auto FSceneRenderer::EnqueueResourceInvalidation(
		ERendererResourceInvalidationCause Cause) -> void
	{
		ENQUEUE_RENDER_COMMAND(InvalidateRendererResources)(
			[this, Cause](FRHICommandListImmediate& CommandList) {
				ApplyResourceInvalidation_RenderThread(CommandList, Cause);
			});
	}

	auto FSceneRenderer::ApplyResourceInvalidation_RenderThread(
		FRHICommandListImmediate& CommandList,
		ERendererResourceInvalidationCause Cause) -> void
	{
		check(IsInRenderingThread());
		Coordinator.Apply_RenderThread(
			Cause,
			{
				.InvalidateShaderResources =
					[](bool) {},
				.ReleaseDeviceResources =
					[this] {
						DefaultTextures.ReleaseResources_RenderThread();
						EnvironmentLighting.ReleaseResources_RenderThread();
						StaticMeshRenderer.ReleaseResources_RenderThread();
						TerrainRenderer.ReleaseResources_RenderThread();
						SkeletalMeshRenderer.ReleaseResources_RenderThread();
						DirectionalShadowRenderer.ReleaseResources_RenderThread();
						SkyBoxRenderer.ReleaseResources_RenderThread();
						PostProcessRenderer.ReleaseResources_RenderThread();
						EditorAssistanceRenderer.
							ReleaseResources_RenderThread();
						FullscreenGeometry.ReleaseResources_RenderThread();
					},
				.RecreateStartupResources =
					[this, &CommandList] {
						check(GDynamicRHI != nullptr);
						DefaultTextures.Initialize_RenderThread(CommandList);
					},
				.RetryFailedResources =
					[this, &CommandList] {
						DefaultTextures.Initialize_RenderThread(CommandList);
						FullscreenGeometry.
							RetryFailedResources_RenderThread();
					},
			});
	}

	auto FSceneRenderer::RenderView_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		const FSceneView& View,
		FRHITexture* OutputTarget,
		bool bPresentOutput,
		const FSceneViewRenderOptions& Options,
		FSceneViewStatistics* OutStatistics) -> ERenderViewResult
	{
		check(IsInRenderingThread());
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderView");
		FPreparedSceneView PreparedView;
		struct FCounterSnapshotScope
		{
			const FViewRenderCounters& Counters;
			FSceneViewStatistics* OutStatistics = nullptr;
			~FCounterSnapshotScope()
			{
				EmitViewRenderCounterSnapshot(Counters);
				if (OutStatistics != nullptr)
					*OutStatistics = BuildSceneViewStatistics(Counters);
			}
		} CounterSnapshotScope{PreparedView.Counters, OutStatistics};
		const uint32 Width =
			OutputTarget != nullptr ? OutputTarget->GetSizeX() : 0;
		const uint32 Height =
			OutputTarget != nullptr ? OutputTarget->GetSizeY() : 0;
		if (OutputTarget == nullptr || Width == 0 || Height == 0)
		{
			return ERenderViewResult::InvalidOutput;
		}
		const RenderTargetLayouts::EViewportOutput ViewportOutput =
			GetViewportOutput(bPresentOutput);
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
		PreparedView.View = RenderView;
		if (Options.Environment)
		{
			const FViewEnvironmentOverride& Environment = *Options.Environment;
			FRHITexture* Texture = Environment.TextureReference != nullptr
				? Environment.TextureReference->GetReferencedTexture_RenderThread()
				: nullptr;
			if (Texture == nullptr
				|| Texture->GetDimension() != ETextureDimension::TextureCube)
			{
				return ERenderViewResult::RequiredEnvironmentUnavailable;
			}
			PreparedView.SkyBox.TextureReference = Environment.TextureReference;
			PreparedView.SkyBox.Rotation = Environment.Rotation;
			PreparedView.SkyBox.Tint = Environment.Tint;
			PreparedView.SkyBox.Intensity = Environment.Intensity;
			PreparedView.ViewEnvironmentTexture = Texture;
			PreparedView.bHasViewEnvironment = true;
			PreparedView.bHasSkyBox = true;
		}
		if (Scene != nullptr)
		{
			const FSceneVisibilityResult Visibility = PrepareSceneVisibility(
				*Scene, RenderView, PreparedView.Counters);
			const FSkyBoxSceneInfo* SkyBoxInfo =
				Scene->GetActiveSkyBoxSceneInfo_RenderThread();
			if (!PreparedView.bHasViewEnvironment && SkyBoxInfo != nullptr)
			{
				PreparedView.SkyBox = SkyBoxInfo->GetProxy().GetData();
				PreparedView.bHasSkyBox = true;
			}
			PreparedView.Lights = PrepareLightView_RenderThread(
				*Scene, RenderView, PreparedView.Counters);
			if (!PreparedView.Lights.Directional.empty())
			{
				++PreparedView.Counters.ShadowSelectedLights;
				const FPreparedDirectionalLight& Selected =
					PreparedView.Lights.Directional.front();
				if (TryPrepareDirectionalShadowView(
						RenderView, Selected.Id, Selected.Data,
						PreparedView.DirectionalShadow))
				{
					++PreparedView.Counters.ShadowValidReceiverViews;
					const size_t DiagnosticIndex = static_cast<size_t>(
						PreparedView.DirectionalShadow.DiagnosticMode);
					if (DiagnosticIndex
						< PreparedView.Counters.ShadowDiagnosticViews.size())
						++PreparedView.Counters.ShadowDiagnosticViews[DiagnosticIndex];
					PreparedView.Counters.ShadowCandidate =
						PreparedView.DirectionalShadow.Candidate;
					PreparedView.Counters.ShadowCascadeCount =
						PreparedView.DirectionalShadow.CascadeCount;
					const FDirectionalShadowFilter& Filter =
						PreparedView.DirectionalShadow.Cascades[0].Filter;
					const size_t QualityIndex = static_cast<size_t>(Filter.Quality);
					if (QualityIndex < PreparedView.Counters.ShadowQualityViews.size())
						++PreparedView.Counters.ShadowQualityViews[QualityIndex];
					PreparedView.Counters.ShadowComparisonOperations +=
						Filter.ComparisonOperations;
					PreparedView.Counters.ShadowTransitionComparisonOperations +=
						PreparedView.DirectionalShadow.CascadeCount > 1
							? 2u * Filter.ComparisonOperations
							: Filter.ComparisonOperations;
					PreparedView.Counters.ShadowGuardTexels +=
						Filter.GuardTexels;
					PreparedView.Counters.ShadowInvalidQualityFallbacks +=
						Filter.bUsedInvalidQualityFallback ? 1u : 0u;
					for (uint32 CascadeIndex = 0;
						CascadeIndex < PreparedView.DirectionalShadow.CascadeCount;
						++CascadeIndex)
					{
						const auto& Cascade =
							PreparedView.DirectionalShadow.Cascades[CascadeIndex];
						auto& CascadeCounters =
							PreparedView.Counters.ShadowCascades[CascadeIndex];
						CascadeCounters.NearDepth = Cascade.NearDepth;
						CascadeCounters.FarDepth = Cascade.FarDepth;
						CascadeCounters.TransitionStartDepth =
							Cascade.TransitionStartDepth;
						CascadeCounters.TexelWorldSizeX = Cascade.TexelWorldSize.x;
						CascadeCounters.TexelWorldSizeY = Cascade.TexelWorldSize.y;
						CascadeCounters.ComparisonOperations =
							Cascade.Filter.ComparisonOperations;
						CascadeCounters.GuardTexels = Cascade.Filter.GuardTexels;
						PreparedView.Counters.ShadowBiasFallbacks +=
							Cascade.Bias.bUsedFallback ? 1u : 0u;
						PreparedView.Counters.ShadowBiasClamps +=
							Cascade.Bias.bTotalClamped ? 1u : 0u;
						const FDirectionalShadowCasterCandidates Casters =
							PrepareDirectionalShadowCasterCandidates(*Scene, Cascade);
						CascadeCounters.SubmittedCasters = Casters.Submitted;
						CascadeCounters.HiddenCasters = Casters.Hidden;
						CascadeCounters.CulledCasters = Casters.Culled;
						CascadeCounters.InvalidBoundsFallbacks =
							Casters.InvalidBoundsFallbacks;
						PreparedView.Counters.ShadowSubmittedCasters += Casters.Submitted;
						PreparedView.Counters.ShadowHiddenCasters += Casters.Hidden;
						PreparedView.Counters.ShadowCulledCasters += Casters.Culled;
						PreparedView.Counters.ShadowInvalidBoundsFallbacks +=
							Casters.InvalidBoundsFallbacks;
						auto& StaticMeshes =
							PreparedView.ShadowStaticMeshes[CascadeIndex];
						auto& SkeletalMeshes =
							PreparedView.ShadowSkeletalMeshes[CascadeIndex];
						auto& Terrains = PreparedView.ShadowTerrains[CascadeIndex];
						StaticMeshes = PrepareStaticMeshView_RenderThread(
							CommandList, Casters.StaticMeshes, Cascade.CasterView,
							ERasterMode::Solid, Casters.SplineMeshes);
						SkeletalMeshes = PrepareSkeletalMeshView_RenderThread(
							CommandList, Casters.SkeletalMeshes, Cascade.CasterView,
							ERasterMode::Solid);
						Terrains = PrepareTerrainView_RenderThread(
							Casters.Terrains, Cascade.CasterView, ERasterMode::Solid);
						auto ApplyRasterBias = [&Cascade](auto& Geometry) {
						for (auto* Bucket : {&Geometry.Opaque, &Geometry.Masked})
							for (auto& Draw : *Bucket)
							{
								auto& Raster = Draw.PipelineKey.Rasterizer;
								Raster.bEnableDepthBias = true;
								Raster.DepthBiasConstantFactor =
									Cascade.Bias.RasterConstant;
								Raster.DepthBiasSlopeFactor =
									Cascade.Bias.RasterSlope;
								Raster.DepthBiasClamp =
									Cascade.Bias.RasterClamp;
							}
						};
						ApplyRasterBias(StaticMeshes);
						ApplyRasterBias(SkeletalMeshes);
						ApplyRasterBias(Terrains);
					// Translucent surfaces never enter the M6 shadow draw lists.
						StaticMeshes.SelectedSections -= StaticMeshes.TranslucentSections;
						StaticMeshes.SelectedTriangles -= StaticMeshes.TranslucentTriangles;
						StaticMeshes.Translucent.clear();
						StaticMeshes.TranslucentSections = 0;
						StaticMeshes.TranslucentTriangles = 0;
						SkeletalMeshes.SelectedSections -= SkeletalMeshes.TranslucentSections;
						SkeletalMeshes.SelectedTriangles -= SkeletalMeshes.TranslucentTriangles;
						SkeletalMeshes.Translucent.clear();
						SkeletalMeshes.TranslucentSections = 0;
						SkeletalMeshes.TranslucentTriangles = 0;
						Terrains.Translucent.clear();
						CascadeCounters.PreparedStaticMeshCasters =
							StaticMeshes.PreparedLocalPrimitives;
						CascadeCounters.PreparedSplineMeshCasters =
							StaticMeshes.PreparedSplinePrimitives;
						CascadeCounters.PreparedSkeletalMeshCasters =
							SkeletalMeshes.Primitives.size();
						CascadeCounters.PreparedTerrainCasters =
							Terrains.Opaque.size() + Terrains.Masked.size();
						size_t TerrainShadowTriangles = 0;
						for (const auto* Bucket : {&Terrains.Opaque, &Terrains.Masked})
							for (const FPreparedTerrainDraw& Draw : *Bucket)
								TerrainShadowTriangles += Draw.TriangleCount;
						CascadeCounters.PreparedTriangles =
							StaticMeshes.SelectedTriangles
							+ SkeletalMeshes.SelectedTriangles + TerrainShadowTriangles;
						PreparedView.Counters.ShadowPreparedStaticMeshCasters +=
							CascadeCounters.PreparedStaticMeshCasters;
						PreparedView.Counters.ShadowPreparedSplineMeshCasters +=
							CascadeCounters.PreparedSplineMeshCasters;
						PreparedView.Counters.ShadowPreparedSkeletalMeshCasters +=
							CascadeCounters.PreparedSkeletalMeshCasters;
						PreparedView.Counters.ShadowPreparedTerrainCasters +=
							CascadeCounters.PreparedTerrainCasters;
						PreparedView.Counters.ShadowPreparedTriangles +=
							CascadeCounters.PreparedTriangles;
					}
				}
				else if (Selected.Data.bCastShadows)
				{
					++PreparedView.Counters.ShadowInvalidReceiverViews;
				}
			}
			PreparedView.StaticMeshes = PrepareStaticMeshView_RenderThread(
				CommandList,
				Visibility.StaticMeshSceneInfos,
				RenderView,
				RenderView.Settings.RasterMode,
				Visibility.SplineMeshSceneInfos);
			PreparedView.SkeletalMeshes = PrepareSkeletalMeshView_RenderThread(
				CommandList, Visibility.SkeletalMeshSceneInfos, RenderView,
				RenderView.Settings.RasterMode);
			PreparedView.Terrains = PrepareTerrainView_RenderThread(
				Visibility.TerrainSceneInfos, RenderView,
				RenderView.Settings.RasterMode);
			if (RenderView.Settings.bShowTerrainLODOverlay)
			{
				auto AddTerrainDrawOverlay = [&RenderView](const FPreparedTerrainDraw& Draw) {
					if (!Draw.SceneInfo || !Draw.Patch) return;
					const FBox& Bounds = Draw.Patch->LocalBounds;
					const FMatrix& Transform = Draw.SceneInfo->GetTransform();
					if (!Bounds.bIsValid || !Math::IsFinite(Transform)) return;
					const std::array<FVector3, 4> Local{
						FVector3{Bounds.Min.x, Bounds.Min.y, Bounds.Max.z},
						FVector3{Bounds.Max.x, Bounds.Min.y, Bounds.Max.z},
						FVector3{Bounds.Max.x, Bounds.Max.y, Bounds.Max.z},
						FVector3{Bounds.Min.x, Bounds.Max.y, Bounds.Max.z}};
					std::array<FVector3, 4> World;
					for (size_t Index = 0; Index < 4; ++Index)
						World[Index] = FVector3(Transform * FVector4(Local[Index], 1.0));
					const float Level = std::min(1.0f, Draw.ResolvedLOD / 6.0f);
					const FVector4f LevelColor{Level, 1.0f - Level, 0.2f, 0.9f};
					for (uint8 Edge = 0; Edge < 4; ++Edge)
					{
						const bool bStitched = (Draw.StitchMask & (1u << Edge)) != 0;
						RenderView.OverlayLines.push_back({
							.Start = World[Edge],
							.End = World[(Edge + 1) % 4],
							.Color = bStitched ? FVector4f{1.0f, 0.1f, 0.1f, 1.0f} : LevelColor,
							.WidthPixels = bStitched ? 3.0f : 2.0f});
					}
				};
				for (const auto* Bucket : {&PreparedView.Terrains.Opaque,
					&PreparedView.Terrains.Masked, &PreparedView.Terrains.Translucent})
					for (const FPreparedTerrainDraw& Draw : *Bucket) AddTerrainDrawOverlay(Draw);
			}
		}
		StaticMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.StaticMeshes);
		SkeletalMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.SkeletalMeshes);
		TerrainRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Terrains);
		DirectionalShadowRenderer.PrepareResources_RenderThread(
			CommandList, StaticMeshRenderer, SkeletalMeshRenderer,
			TerrainRenderer, PreparedView);
		FRHITexture* DirectionalShadowTexture =
			DirectionalShadowRenderer.GetTexture_RenderThread();
		FRHISampler* DirectionalShadowSampler =
			DirectionalShadowRenderer.GetSampler_RenderThread();
		auto BindShadow = [DirectionalShadowTexture, DirectionalShadowSampler](
			auto& PreparedGeometry) {
			for (auto* Bucket : {&PreparedGeometry.Opaque,
				&PreparedGeometry.Masked, &PreparedGeometry.Translucent})
				for (auto& Draw : *Bucket)
				{
					Draw.DirectionalShadowTexture = DirectionalShadowTexture;
					Draw.DirectionalShadowSampler = DirectionalShadowSampler;
				}
		};
		BindShadow(PreparedView.StaticMeshes);
		BindShadow(PreparedView.SkeletalMeshes);
		BindShadow(PreparedView.Terrains);
		PrepareCombinedTranslucentGeometry(PreparedView);
		PreparedView.Counters.CombinedTranslucentGeometryDraws =
			PreparedView.TranslucentGeometry.size();
		CopyStaticMeshCounters(
			PreparedView.StaticMeshes, PreparedView.Counters);
		CopySkeletalMeshCounters(
			PreparedView.SkeletalMeshes, PreparedView.Counters);
		CopyTerrainCounters(PreparedView.Terrains, PreparedView.Counters);
		const FForwardLightingUniform Lighting = BuildForwardLightingUniform(
			PreparedView.Lights, RenderView,
			PreparedView.DirectionalShadow.bEnabled
				&& DirectionalShadowTexture != nullptr
				&& DirectionalShadowSampler != nullptr
				? &PreparedView.DirectionalShadow : nullptr);
		PreparedView.Counters.PackedLightBytes = sizeof(Lighting);
		PreparedView.LightingUniformBuffer =
			CommandList.AllocateDynamicUniformBuffer(&Lighting, sizeof(Lighting));
		if (PreparedView.LightingUniformBuffer.Buffer == nullptr
			|| PreparedView.LightingUniformBuffer.Size != sizeof(Lighting))
		{
			return ERenderViewResult::RendererResourcesUnavailable;
		}

		// Every enabled view regenerates the shared fixed target before Scene Color.
		DirectionalShadowRenderer.Render_RenderThread(
			CommandList, StaticMeshRenderer, SkeletalMeshRenderer,
			TerrainRenderer, PreparedView);

		FRHIRenderPassInfo ScenePassInfo{};
		ScenePassInfo.RenderTargetLayout =
			RenderTargetLayouts::MakeSceneTargets();
		ScenePassInfo.ColorRenderTargets[0] = SceneColor;
		ScenePassInfo.DepthStencilRenderTarget = SceneTargets->Depth;
		ScenePassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r,
			View.ClearColor.g,
			View.ClearColor.b,
			View.ClearColor.a);
		ScenePassInfo.DepthStencilClearValue = FClearValueBinding(1.0f, 0u);
		FGPUTimingQueryRHIRef SceneColorTimingQuery;
		const FSceneColorTimingQuerySink TimingSink =
			GSceneColorTimingQuerySink.load(std::memory_order_acquire);
		if (TimingSink != nullptr && GDynamicRHI != nullptr)
		{
			SceneColorTimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (SceneColorTimingQuery)
				CommandList.BeginGPUTimingQuery(SceneColorTimingQuery);
		}
		CommandList.BeginRenderPass(
			ScenePassInfo,
			"SceneColorRenderPass");
		const bool bSceneRendered =
			RenderScene_RenderThread(CommandList, PreparedView, SceneColor);
		CommandList.EndRenderPass();
		if (SceneColorTimingQuery)
		{
			CommandList.EndGPUTimingQuery(SceneColorTimingQuery);
			TimingSink(SceneColorTimingQuery);
		}
		if (!bSceneRendered)
		{
			return ERenderViewResult::RequiredEnvironmentUnavailable;
		}
		CopyStaticMeshCounters(
			PreparedView.StaticMeshes, PreparedView.Counters);
		CopySkeletalMeshCounters(
			PreparedView.SkeletalMeshes, PreparedView.Counters);
		CopyTerrainCounters(PreparedView.Terrains, PreparedView.Counters);

		const RendererEditorAssistance::FRequest EditorAssistanceRequest =
			FEditorAssistanceRenderer::AnalyzeRequest(RenderView, ViewportOutput);
		RendererEditorAssistance::FPrepared PreparedEditorAssistance;
		if (!EditorAssistanceRequest.IsEmpty())
		{
			PreparedEditorAssistance =
				EditorAssistanceRenderer.Prepare_RenderThread(
					CommandList,
					RenderView,
					EditorAssistanceRequest);
		}
		const bool bHasEditorAssistance =
			PreparedEditorAssistance.HasDrawableOperation();

		FRHIRenderPassInfo PostProcessPassInfo{};
		PostProcessPassInfo.RenderTargetLayout = bHasEditorAssistance
			? RenderTargetLayouts::MakeScenePostProcessOutput()
			: RenderTargetLayouts::MakeFinalScenePostProcessOutput(
				ViewportOutput);
		PostProcessPassInfo.ColorRenderTargets[0] = OutputTarget;
		PostProcessPassInfo.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r,
			View.ClearColor.g,
			View.ClearColor.b,
			View.ClearColor.a);
		CommandList.BeginRenderPass(
			PostProcessPassInfo,
			bPresentOutput
				? "PostProcessPresentRenderPass"
				: "PostProcessOffscreenRenderPass");
		PostProcessRenderer.Draw_RenderThread(
			CommandList,
			SceneColor,
			Width,
			Height,
			bPresentOutput,
			View.Settings.bEnableFXAA,
			bHasEditorAssistance);
		CommandList.EndRenderPass();
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
			bPresentOutput
				? "EditorAssistancePresentRenderPass"
				: "EditorAssistanceOffscreenRenderPass");
		EditorAssistanceRenderer.Draw_RenderThread(
			CommandList,
			RenderView,
			PreparedEditorAssistance);
		CommandList.EndRenderPass();
		return ERenderViewResult::Success;
	}

	auto FSceneRenderer::RenderScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSceneView& PreparedView,
		FRHITexture* RenderTarget) -> bool
	{
		check(IsInRenderingThread());
		check(CommandList.IsInsideRenderPass());
		DURIN_PROFILE_CPU_ZONE_NAMED("Renderer.RenderScene");
		const FSceneView& View = PreparedView.View;
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
			1.0f);
		CommandList.SetScissor(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(Width),
			static_cast<float>(Height));

		if (PreparedView.bHasSkyBox)
		{
			if (PreparedView.bHasViewEnvironment)
			{
				if (!SkyBoxRenderer.DrawTexture_RenderThread(
						CommandList,
						View,
						PreparedView.ViewEnvironmentTexture,
						PreparedView.SkyBox))
				{
					return false;
				}
			}
			else
			{
				SkyBoxRenderer.Draw_RenderThread(
					CommandList, View, PreparedView.SkyBox);
			}
		}

		for (const EStaticMeshBasePass Pass : {
			EStaticMeshBasePass::Opaque, EStaticMeshBasePass::Masked})
		{
			StaticMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.LightingUniformBuffer,
				View.Settings.RenderMode, Pass, PreparedView.StaticMeshes);
			SkeletalMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.LightingUniformBuffer,
				View.Settings.RenderMode, Pass, PreparedView.SkeletalMeshes);
			TerrainRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.LightingUniformBuffer,
				View.Settings.RenderMode, Pass, PreparedView.Terrains);
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 PreparedView.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.StaticMeshes);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.SkeletalMeshes);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode,
					PreparedView.Terrains.Translucent[Draw.DrawIndex],
					PreparedView.Terrains);
		}
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			PreparedView.StaticMeshes);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			PreparedView.SkeletalMeshes);
		TerrainRenderer.FinalizeExecution_RenderThread(PreparedView.Terrains);
		return true;
	}
} // namespace Durin
