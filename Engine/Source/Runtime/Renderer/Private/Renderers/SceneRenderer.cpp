#include "Renderers/SceneRenderer.h"

#include "Engine/TerrainSceneProxy.h"
#include "Renderers/PreparedSceneView.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/TerrainRenderPreparation.h"
#include "Renderers/SceneRendererProfiling.h"

#include "Profiling/Profiling.h"

#include "AssetLoad.h"
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
		std::atomic<FPostProcessTimingQuerySink> GPostProcessTimingQuerySink = nullptr;
		std::atomic<FGBufferTimingQuerySink> GGBufferTimingQuerySink = nullptr;
		std::atomic<FDeferredDirectionalTimingQuerySink>
			GDeferredDirectionalTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionTimingQuerySink>
			GGroundTruthAmbientOcclusionTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionFilterTimingQuerySink>
			GGroundTruthAmbientOcclusionFilterTimingQuerySink = nullptr;
		std::atomic<FHDRSceneColorCaptureSink> GHDRSceneColorCaptureSink = nullptr;
		std::atomic<FGBufferCaptureSink> GGBufferCaptureSink = nullptr;
		std::atomic<FDeferredDirectionalCaptureSink>
			GDeferredDirectionalCaptureSink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionCaptureSink>
			GGroundTruthAmbientOcclusionCaptureSink = nullptr;

		auto AddSaturated(uint64 A, uint64 B) -> uint64
		{
			return B > std::numeric_limits<uint64>::max() - A ? std::numeric_limits<uint64>::max() : A + B;
		}

		auto GetViewportOutput(bool bPresent)
			-> RenderTargetLayouts::EViewportOutput
		{
			return bPresent ? RenderTargetLayouts::EViewportOutput::Present : RenderTargetLayouts::EViewportOutput::Offscreen;
		}

		auto CopyStaticMeshCounters(
			const FPreparedStaticMeshView& StaticMeshes,
			FViewRenderCounters& Counters
		) -> void
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
			FViewRenderCounters& Counters
		) -> void
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
			const FPreparedTerrainView& Terrain, FViewRenderCounters& Counters
		) -> void
		{
			Counters.TerrainPatchCandidates = Terrain.PatchCandidates;
			Counters.VisibleTerrainPatches = Terrain.VisiblePatches;
			Counters.CulledTerrainPatches = Terrain.CulledPatches;
			Counters.InnerTerrainPatches = Terrain.InnerPatches;
			Counters.TransitionTerrainPatches = Terrain.TransitionPatches;
			Counters.RadialRejectedTerrainPatches = Terrain.RadialRejectedPatches;
			Counters.InvalidTerrainDistanceSettingFallbacks =
				Terrain.InvalidDistanceSettingFallbacks;
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
									 - std::min(Counters.PreparedStaticMeshTriangles, Counters.PreparedSplineMeshTriangles);
		Result.SkeletalMeshTriangles = Counters.PreparedSkeletalMeshTriangles;
		Result.TerrainTriangles = Counters.PreparedTerrainTriangles;
		Result.Triangles = AddSaturated(
			AddSaturated(Result.StaticMeshTriangles, Result.SplineMeshTriangles),
			AddSaturated(Result.SkeletalMeshTriangles, Result.TerrainTriangles)
		);
		Result.ShadowTriangles = Counters.ShadowPreparedTriangles;

		Result.StaticMeshDrawCalls = Counters.StaticMeshSuccessfulDraws;
		Result.SkeletalMeshDrawCalls = Counters.SkeletalMeshSuccessfulDraws;
		Result.TerrainDrawCalls = Counters.TerrainSuccessfulDraws;
		Result.ShadowDrawCalls = Counters.ShadowSuccessfulDraws;
		Result.DirectionalLights = Counters.SelectedDirectionalLights;
		Result.PointLights = Counters.SelectedPointLights;
		Result.SpotLights = Counters.SelectedSpotLights;
		Result.ShadowCascades = static_cast<uint32>(std::min<size_t>(
			Counters.ShadowCascadeCount, std::numeric_limits<uint32>::max()
		));
		Result.bShadowEnabled = Counters.ShadowValidReceiverViews != 0
								&& Counters.ShadowCascadeCount != 0;
		Result.bContactShadowEnabled = Counters.ContactShadowEnabledViews != 0;
		return Result;
	}

	auto SetSceneColorTimingQuerySink(FSceneColorTimingQuerySink Sink) -> void
	{
		GSceneColorTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto SetPostProcessTimingQuerySink(FPostProcessTimingQuerySink Sink) -> void
	{
		GPostProcessTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto SetGBufferTimingQuerySink(FGBufferTimingQuerySink Sink) -> void
	{
		GGBufferTimingQuerySink.store(Sink, std::memory_order_release);
	}

	auto SetDeferredDirectionalTimingQuerySink(
		FDeferredDirectionalTimingQuerySink Sink
	) -> void
	{
		GDeferredDirectionalTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionTimingQuerySink(
		FGroundTruthAmbientOcclusionTimingQuerySink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionFilterTimingQuerySink(
		FGroundTruthAmbientOcclusionFilterTimingQuerySink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionFilterTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetHDRSceneColorCaptureSink(FHDRSceneColorCaptureSink Sink) -> void
	{
		GHDRSceneColorCaptureSink.store(Sink, std::memory_order_release);
	}

	auto SetGBufferCaptureSink(FGBufferCaptureSink Sink) -> void
	{
		GGBufferCaptureSink.store(Sink, std::memory_order_release);
	}

	auto SetDeferredDirectionalCaptureSink(
		FDeferredDirectionalCaptureSink Sink
	) -> void
	{
		GDeferredDirectionalCaptureSink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionCaptureSink(
		FGroundTruthAmbientOcclusionCaptureSink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionCaptureSink.store(
			Sink, std::memory_order_release
		);
	}

	FSceneRenderer::FSceneRenderer()
		: DefaultTextures(Coordinator)
		, EnvironmentLighting(Coordinator)
		, DirectionalShadowRenderer(Coordinator)
		, GBufferRenderer(Coordinator)
		, GBufferDebugRenderer(Coordinator, FullscreenGeometry)
		, DeferredDirectionalLightingRenderer(Coordinator, FullscreenGeometry)
		, GroundTruthAmbientOcclusionRenderer(Coordinator, FullscreenGeometry)
		, StaticMeshRenderer(Coordinator, DefaultTextures, EnvironmentLighting)
		, TerrainRenderer(Coordinator, DefaultTextures, EnvironmentLighting)
		, SkeletalMeshRenderer(Coordinator, DefaultTextures, EnvironmentLighting)
		, SkyBoxRenderer(Coordinator, DefaultTextures)
		, PostProcessRenderer(Coordinator, FullscreenGeometry)
		, ContactShadowRenderer(Coordinator, FullscreenGeometry)
		, EditorAssistanceRenderer(Coordinator, FullscreenGeometry)
	{
	}

	FSceneRenderer::~FSceneRenderer() = default;

	auto FSceneRenderer::Start(
		FConsoleCommandRegistry& Registry,
		FModuleOwnedCallbackGate OwnerGate
	) -> bool
	{
		FAssetPath EnvironmentPath;
		DEnvironmentLighting* EnvironmentAsset = nullptr;
		std::string PathError;
		Asset::FAssetResult EnvironmentResult =
			FAssetPath::TryCreate(
				"/Engine/Renderer/DefaultStudioEnvironment",
				EnvironmentPath,
				&PathError
			) ?
				Asset::LoadAsset(EnvironmentPath, EnvironmentAsset) :
				Asset::FAssetResult{Asset::EAssetError::InvalidPath, std::move(PathError)};
		if (EnvironmentResult && EnvironmentAsset != nullptr)
		{
			EnvironmentLighting.Initialize(EnvironmentAsset->GetData());
		}
		else
		{
			DURIN_ERROR(
				"Failed to load the built-in studio environment: {}",
				EnvironmentResult.Message
			);
		}
		return Coordinator.Start(
			Registry,
			[this](ERendererResourceInvalidationCause Cause) {
				EnqueueResourceInvalidation(Cause);
			},
			std::move(OwnerGate)
		);
	}

	auto FSceneRenderer::Stop() -> void
	{
		Coordinator.Stop();
	}

	auto FSceneRenderer::InitializeStartupResources_RenderThread(
		FRHICommandListImmediate& CommandList
	) -> void
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
		GBufferRenderer.ReleaseResources_RenderThread();
		GBufferDebugRenderer.ReleaseResources_RenderThread();
		DeferredDirectionalLightingRenderer.ReleaseResources_RenderThread();
		GroundTruthAmbientOcclusionRenderer.ReleaseResources_RenderThread();
		Coordinator.ReleaseResources_RenderThread();
		SkyBoxRenderer.ReleaseResources_RenderThread();
		EditorAssistanceRenderer.ReleaseResources_RenderThread();
		PostProcessRenderer.ReleaseResources_RenderThread();
		ContactShadowRenderer.ReleaseResources_RenderThread();
		FullscreenGeometry.ReleaseResources_RenderThread();
	}

	auto FSceneRenderer::FitViewToOutput(
		const FSceneView& View,
		uint32 Width,
		uint32 Height
	) -> FSceneView
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
			std::round(ContentWidth / RenderView.AspectRatioConstraint)
		);
		if (ContentHeight > Height)
		{
			ContentHeight = Height;
			ContentWidth = static_cast<uint32>(
				std::round(
					ContentHeight * RenderView.AspectRatioConstraint
				)
			);
		}
		RenderView.ViewportWidth = std::max(1u, ContentWidth);
		RenderView.ViewportHeight = std::max(1u, ContentHeight);
		RenderView.ViewportX = (Width - RenderView.ViewportWidth) / 2;
		RenderView.ViewportY = (Height - RenderView.ViewportHeight) / 2;
		return RenderView;
	}

	auto FSceneRenderer::EnqueueResourceInvalidation(
		ERendererResourceInvalidationCause Cause
	) -> void
	{
		ENQUEUE_RENDER_COMMAND(InvalidateRendererResources)(
			[this, Cause](FRHICommandListImmediate& CommandList) {
				ApplyResourceInvalidation_RenderThread(CommandList, Cause);
			}
		);
	}

	auto FSceneRenderer::ApplyResourceInvalidation_RenderThread(
		FRHICommandListImmediate& CommandList,
		ERendererResourceInvalidationCause Cause
	) -> void
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
						GBufferRenderer.ReleaseResources_RenderThread();
						GBufferDebugRenderer.ReleaseResources_RenderThread();
						DeferredDirectionalLightingRenderer.ReleaseResources_RenderThread();
						GroundTruthAmbientOcclusionRenderer.ReleaseResources_RenderThread();
						SkyBoxRenderer.ReleaseResources_RenderThread();
						PostProcessRenderer.ReleaseResources_RenderThread();
						ContactShadowRenderer.ReleaseResources_RenderThread();
						EditorAssistanceRenderer.ReleaseResources_RenderThread();
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
						FullscreenGeometry.RetryFailedResources_RenderThread();
					},
			}
		);
	}

	auto FSceneRenderer::RenderView_RenderThread(
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
			|| SceneTargets->Depth == nullptr
			|| SceneTargets->DirectionalDirect == nullptr)
		{
			return ERenderViewResult::RendererResourcesUnavailable;
		}
		FRHITexture* SceneColor = SceneTargets->Color;

		FSceneView RenderView = FitViewToOutput(View, Width, Height);
		PreparedView.View = RenderView;
		if (Options.Environment)
		{
			const FViewEnvironmentOverride& Environment = *Options.Environment;
			FRHITexture* Texture = Environment.TextureReference != nullptr ? Environment.TextureReference->GetReferencedTexture_RenderThread() : nullptr;
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
				*Scene, RenderView, PreparedView.Counters
			);
			const FSkyBoxSceneInfo* SkyBoxInfo =
				Scene->GetActiveSkyBoxSceneInfo_RenderThread();
			if (!PreparedView.bHasViewEnvironment && SkyBoxInfo != nullptr)
			{
				PreparedView.SkyBox = SkyBoxInfo->GetProxy().GetData();
				PreparedView.bHasSkyBox = true;
			}
			PreparedView.Lights = PrepareLightView_RenderThread(
				*Scene, RenderView, PreparedView.Counters
			);
			if (!PreparedView.Lights.Directional.empty())
			{
				++PreparedView.Counters.ShadowSelectedLights;
				const FPreparedDirectionalLight& Selected =
					PreparedView.Lights.Directional.front();
				if (TryPrepareDirectionalShadowView(
						RenderView, Selected.Id, Selected.Data,
						PreparedView.DirectionalShadow
					))
				{
					++PreparedView.Counters.ShadowValidReceiverViews;
					const size_t DiagnosticIndex = static_cast<size_t>(
						PreparedView.DirectionalShadow.DiagnosticMode
					);
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
						PreparedView.DirectionalShadow.CascadeCount > 1 ? 2u * Filter.ComparisonOperations : Filter.ComparisonOperations;
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
							ERasterMode::Solid, Casters.SplineMeshes
						);
						SkeletalMeshes = PrepareSkeletalMeshView_RenderThread(
							CommandList, Casters.SkeletalMeshes, Cascade.CasterView,
							ERasterMode::Solid
						);
						Terrains = PrepareTerrainView_RenderThread(
							Casters.Terrains, Cascade.CasterView, ERasterMode::Solid
						);
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
				Visibility.SplineMeshSceneInfos
			);
			PreparedView.SkeletalMeshes = PrepareSkeletalMeshView_RenderThread(
				CommandList, Visibility.SkeletalMeshSceneInfos, RenderView,
				RenderView.Settings.RasterMode
			);
			PreparedView.Terrains = PrepareTerrainView_RenderThread(
				Visibility.TerrainSceneInfos, RenderView,
				RenderView.Settings.RasterMode
			);
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
						FVector3{Bounds.Min.x, Bounds.Max.y, Bounds.Max.z}
					};
					std::array<FVector3, 4> World;
					for (size_t Index = 0; Index < 4; ++Index)
						World[Index] = FVector3(Transform * FVector4(Local[Index], 1.0));
					const float Level = std::min(1.0f, Draw.ResolvedLOD / 6.0f);
					const FVector4f LevelColor{Level, 1.0f - Level, 0.2f, 0.9f};
					for (uint8 Edge = 0; Edge < 4; ++Edge)
					{
						const bool bStitched = (Draw.StitchMask & (1u << Edge)) != 0;
						RenderView.OverlayLines.push_back({.Start = World[Edge], .End = World[(Edge + 1) % 4], .Color = bStitched ? FVector4f{1.0f, 0.1f, 0.1f, 1.0f} : LevelColor, .WidthPixels = bStitched ? 3.0f : 2.0f});
					}
				};
				for (const auto* Bucket : {&PreparedView.Terrains.Opaque, &PreparedView.Terrains.Masked, &PreparedView.Terrains.Translucent})
					for (const FPreparedTerrainDraw& Draw : *Bucket)
						AddTerrainDrawOverlay(Draw);
			}
		}
		const bool bRequiresDeferredOpaque =
			RenderView.Settings.RenderMode == ERenderMode::Lit
			&& RenderView.Settings.RasterMode == ERasterMode::Solid;
		StaticMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.StaticMeshes, !bRequiresDeferredOpaque
		);
		SkeletalMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.SkeletalMeshes, !bRequiresDeferredOpaque
		);
		TerrainRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.Terrains, !bRequiresDeferredOpaque
		);
		DirectionalShadowRenderer.PrepareResources_RenderThread(
			CommandList, StaticMeshRenderer, SkeletalMeshRenderer,
			TerrainRenderer, PreparedView
		);
		FRHITexture* DirectionalShadowTexture =
			DirectionalShadowRenderer.GetTexture_RenderThread();
		FRHISampler* DirectionalShadowSampler =
			DirectionalShadowRenderer.GetSampler_RenderThread();
		auto BindShadow = [DirectionalShadowTexture, DirectionalShadowSampler](
							  auto& PreparedGeometry
						  ) {
			for (auto* Bucket : {&PreparedGeometry.Opaque, &PreparedGeometry.Masked, &PreparedGeometry.Translucent})
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
			PreparedView.StaticMeshes, PreparedView.Counters
		);
		CopySkeletalMeshCounters(
			PreparedView.SkeletalMeshes, PreparedView.Counters
		);
		CopyTerrainCounters(PreparedView.Terrains, PreparedView.Counters);
		const FForwardLightingUniform Lighting = BuildForwardLightingUniform(
			PreparedView.Lights, RenderView,
			PreparedView.DirectionalShadow.bEnabled
					&& DirectionalShadowTexture != nullptr
					&& DirectionalShadowSampler != nullptr ?
				&PreparedView.DirectionalShadow :
				nullptr
		);
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
			TerrainRenderer, PreparedView
		);

		FGBufferRenderer::FTargets* GBufferTargets = nullptr;
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
			&& RenderView.Settings.bEnableGroundTruthAmbientOcclusion;
		const bool bWantsGroundTruthAmbientOcclusion =
			bWantsIsolatedGroundTruthAmbientOcclusion
			|| bWantsProductionGroundTruthAmbientOcclusion;
		const bool bWantsDeferredInputs = bWantsIsolatedDeferred
										  || bWantsProductionDeferred
										  || bWantsGroundTruthAmbientOcclusion;
		const bool bHybridRetainedResourcesReady =
			!bWantsProductionDeferred
			|| (StaticMeshRenderer.PrepareHybridRetainedResources_RenderThread(
					PreparedView.StaticMeshes
				)
				&& SkeletalMeshRenderer.PrepareHybridRetainedResources_RenderThread(
					PreparedView.SkeletalMeshes
				)
				&& TerrainRenderer.PrepareHybridRetainedResources_RenderThread(
					CommandList, PreparedView.Terrains
				));
		const bool bNeedsGBuffer = Options.bEnableGBufferQualification
								   || Options.GBufferDebugMode != EGBufferDebugMode::Disabled
								   || bWantsDeferredInputs;
		if (bNeedsGBuffer)
		{
			GBufferTargets =
				GBufferRenderer.EnsureTargets_RenderThread(Width, Height);
			if (GBufferTargets == nullptr)
			{
				++PreparedView.Counters.GBufferUnavailableViews;
				if (bWantsIsolatedDeferred)
					++PreparedView.Counters.DeferredDirectionalUnavailableViews;
				if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled)
					++PreparedView.Counters.GBufferDebugFailures;
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
					View.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f,
					0u
				);
				FGPUTimingQueryRHIRef GBufferTimingQuery;
				const FGBufferTimingQuerySink GBufferTimingSink =
					GGBufferTimingQuerySink.load(std::memory_order_acquire);
				if (GBufferTimingSink != nullptr && GDynamicRHI != nullptr)
				{
					GBufferTimingQuery =
						GDynamicRHI->RHICreateGPUTimingQuery();
					if (GBufferTimingQuery)
						CommandList.BeginGPUTimingQuery(GBufferTimingQuery);
				}
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
				StaticMeshRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					PreparedView.StaticMeshes
				);
				SkeletalMeshRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					PreparedView.SkeletalMeshes
				);
				TerrainRenderer.ExecuteGBuffer_RenderThread(
					CommandList, RenderView, GBufferRenderer,
					PreparedView.Terrains
				);
				CommandList.EndRenderPass();
				if (GBufferTimingQuery)
				{
					CommandList.EndGPUTimingQuery(GBufferTimingQuery);
					GBufferTimingSink(GBufferTimingQuery);
				}
				const FGBufferCaptureSink GBufferCaptureSink =
					GGBufferCaptureSink.load(std::memory_order_acquire);
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
				++PreparedView.Counters.GBufferEnabledViews;
				PreparedView.Counters.GBufferAttachmentBytes =
					FGBufferRenderer::CalculateTargetBytes(Width, Height);
				PreparedView.Counters.GBufferAttemptedDraws =
					PreparedView.StaticMeshes.GBufferAttemptedDraws
					+ PreparedView.SkeletalMeshes.GBufferAttemptedDraws
					+ PreparedView.Terrains.GBufferAttemptedDraws;
				PreparedView.Counters.GBufferSuccessfulDraws =
					PreparedView.StaticMeshes.GBufferSuccessfulDraws
					+ PreparedView.SkeletalMeshes.GBufferSuccessfulDraws
					+ PreparedView.Terrains.GBufferSuccessfulDraws;
				PreparedView.Counters.GBufferRejectedDraws =
					PreparedView.StaticMeshes.GBufferRejectedDraws
					+ PreparedView.SkeletalMeshes.GBufferRejectedDraws
					+ PreparedView.Terrains.GBufferRejectedDraws;
				PreparedView.Counters.GBufferSkippedDraws =
					PreparedView.StaticMeshes.GBufferSkippedDraws
					+ PreparedView.SkeletalMeshes.GBufferSkippedDraws
					+ PreparedView.Terrains.GBufferSkippedDraws;
				PreparedView.Counters.GBufferStaticMeshAttemptedDraws =
					PreparedView.StaticMeshes.GBufferLocalAttemptedDraws;
				PreparedView.Counters.GBufferStaticMeshSuccessfulDraws =
					PreparedView.StaticMeshes.GBufferLocalSuccessfulDraws;
				PreparedView.Counters.GBufferStaticMeshRejectedDraws =
					PreparedView.StaticMeshes.GBufferLocalRejectedDraws;
				PreparedView.Counters.GBufferStaticMeshSkippedDraws =
					PreparedView.StaticMeshes.GBufferLocalSkippedDraws;
				PreparedView.Counters.GBufferSplineMeshAttemptedDraws =
					PreparedView.StaticMeshes.GBufferSplineAttemptedDraws;
				PreparedView.Counters.GBufferSplineMeshSuccessfulDraws =
					PreparedView.StaticMeshes.GBufferSplineSuccessfulDraws;
				PreparedView.Counters.GBufferSplineMeshRejectedDraws =
					PreparedView.StaticMeshes.GBufferSplineRejectedDraws;
				PreparedView.Counters.GBufferSplineMeshSkippedDraws =
					PreparedView.StaticMeshes.GBufferSplineSkippedDraws;
				PreparedView.Counters.GBufferSkeletalMeshAttemptedDraws =
					PreparedView.SkeletalMeshes.GBufferAttemptedDraws;
				PreparedView.Counters.GBufferSkeletalMeshSuccessfulDraws =
					PreparedView.SkeletalMeshes.GBufferSuccessfulDraws;
				PreparedView.Counters.GBufferSkeletalMeshRejectedDraws =
					PreparedView.SkeletalMeshes.GBufferRejectedDraws;
				PreparedView.Counters.GBufferSkeletalMeshSkippedDraws =
					PreparedView.SkeletalMeshes.GBufferSkippedDraws;
				PreparedView.Counters.GBufferTerrainAttemptedDraws =
					PreparedView.Terrains.GBufferAttemptedDraws;
				PreparedView.Counters.GBufferTerrainSuccessfulDraws =
					PreparedView.Terrains.GBufferSuccessfulDraws;
				PreparedView.Counters.GBufferTerrainRejectedDraws =
					PreparedView.Terrains.GBufferRejectedDraws;
				PreparedView.Counters.GBufferTerrainSkippedDraws =
					PreparedView.Terrains.GBufferSkippedDraws;
			}
		}

		bool bGBufferComplete = false;
		FDeferredDirectionalLightingRenderer::FRenderParameters DeferredParameters;
		FRHITexture* GroundTruthAmbientOcclusionFallback =
			DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		FRHITexture* GroundTruthAmbientOcclusionDebugOutput = nullptr;
		if (bWantsDeferredInputs && GBufferTargets != nullptr)
		{
			bGBufferComplete =
				PreparedView.Counters.GBufferAttemptedDraws
					== PreparedView.Counters.GBufferSuccessfulDraws
				&& PreparedView.Counters.GBufferRejectedDraws == 0;
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
					.Lighting = PreparedView.LightingUniformBuffer,
					.View = &RenderView,
					.DiagnosticMode = static_cast<uint32>(
						Options.DeferredDirectionalDebugMode
					)
				};
			}
		}

		if (bWantsGroundTruthAmbientOcclusion)
		{
			++PreparedView.Counters.GroundTruthAmbientOcclusionAttemptedViews;
			auto* AmbientOcclusionTargets = bGBufferComplete ? GroundTruthAmbientOcclusionRenderer.EnsureTargets_RenderThread(
																   Width, Height
															   ) :
															   nullptr;
			PreparedView.Counters.GroundTruthAmbientOcclusionRetainedBytes =
				GroundTruthAmbientOcclusionRenderer.GetRetainedTargetBytes_RenderThread();
			if (AmbientOcclusionTargets == nullptr)
			{
				++PreparedView.Counters.GroundTruthAmbientOcclusionUnavailableViews;
			}
			else
			{
				FGPUTimingQueryRHIRef TimingQuery;
				const FGroundTruthAmbientOcclusionTimingQuerySink TimingSink =
					GGroundTruthAmbientOcclusionTimingQuerySink.load(
						std::memory_order_acquire
					);
				if (TimingSink != nullptr && GDynamicRHI != nullptr)
				{
					TimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
					if (TimingQuery)
						CommandList.BeginGPUTimingQuery(TimingQuery);
				}
				const bool bRendered =
					GroundTruthAmbientOcclusionRenderer.RenderRaw_RenderThread(
						CommandList, *AmbientOcclusionTargets,
						GBufferTargets->Normals, GBufferTargets->Surface,
						SceneTargets->Depth, RenderView
					);
				if (TimingQuery)
					CommandList.EndGPUTimingQuery(TimingQuery);
				if (bRendered)
				{
					if (TimingQuery)
						TimingSink(TimingQuery);
					const FGroundTruthAmbientOcclusionCaptureSink CaptureSink =
						GGroundTruthAmbientOcclusionCaptureSink.load(
							std::memory_order_acquire
						);
					if (CaptureSink != nullptr)
						CaptureSink(
							CommandList, AmbientOcclusionTargets->Raw, false
						);

					FGPUTimingQueryRHIRef FilterTimingQuery;
					const FGroundTruthAmbientOcclusionFilterTimingQuerySink
						FilterTimingSink =
							GGroundTruthAmbientOcclusionFilterTimingQuerySink.load(
								std::memory_order_acquire
							);
					if (FilterTimingSink != nullptr && GDynamicRHI != nullptr)
					{
						FilterTimingQuery =
							GDynamicRHI->RHICreateGPUTimingQuery();
						if (FilterTimingQuery)
							CommandList.BeginGPUTimingQuery(FilterTimingQuery);
					}
					const bool bFiltered =
						GroundTruthAmbientOcclusionRenderer.RenderFilter_RenderThread(
							CommandList, *AmbientOcclusionTargets,
							GBufferTargets->Normals, GBufferTargets->Surface,
							SceneTargets->Depth, RenderView
						);
					if (FilterTimingQuery)
						CommandList.EndGPUTimingQuery(FilterTimingQuery);
					if (bFiltered)
					{
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
						DeferredParameters.bGroundTruthAmbientOcclusionEnabled = true;
						++PreparedView.Counters.GroundTruthAmbientOcclusionEnabledViews;
						PreparedView.Counters.GroundTruthAmbientOcclusionActiveBytes =
							FGroundTruthAmbientOcclusionRenderer::
								CalculateTargetBytes(Width, Height);
						if (Options.GroundTruthAmbientOcclusionDebugMode
							!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
						{
							++PreparedView.Counters.GroundTruthAmbientOcclusionDebugViews;
						}
						if (FilterTimingQuery)
							FilterTimingSink(FilterTimingQuery);
						if (CaptureSink != nullptr)
							CaptureSink(
								CommandList, AmbientOcclusionTargets->Raw, true
							);
					}
					else
					{
						++PreparedView.Counters.GroundTruthAmbientOcclusionFilterPassFailures;
					}
				}
				else
				{
					++PreparedView.Counters.GroundTruthAmbientOcclusionRawPassFailures;
				}
			}
		}

		if (bWantsIsolatedDeferred)
		{
			auto* DeferredTargets = bGBufferComplete ? DeferredDirectionalLightingRenderer.EnsureTargets_RenderThread(
														   Width, Height
													   ) :
													   nullptr;
			if (DeferredTargets == nullptr)
				++PreparedView.Counters.DeferredDirectionalUnavailableViews;
			else
			{
				DeferredParameters.GroundTruthAmbientOcclusionDebugMode =
					static_cast<uint32>(
						Options.GroundTruthAmbientOcclusionDebugMode
					);
				FGPUTimingQueryRHIRef DeferredTimingQuery;
				const FDeferredDirectionalTimingQuerySink DeferredTimingSink =
					GDeferredDirectionalTimingQuerySink.load(
						std::memory_order_acquire
					);
				if (DeferredTimingSink != nullptr && GDynamicRHI != nullptr)
				{
					DeferredTimingQuery =
						GDynamicRHI->RHICreateGPUTimingQuery();
					if (DeferredTimingQuery)
						CommandList.BeginGPUTimingQuery(DeferredTimingQuery);
				}
				const bool bRendered =
					DeferredDirectionalLightingRenderer.Render_RenderThread(
						CommandList, *DeferredTargets, DeferredParameters
					);
				if (DeferredTimingQuery)
					CommandList.EndGPUTimingQuery(DeferredTimingQuery);
				if (bRendered)
				{
					++PreparedView.Counters.DeferredDirectionalEnabledViews;
					PreparedView.Counters.DeferredDirectionalOutputBytes =
						FDeferredDirectionalLightingRenderer::
							CalculateTargetBytes(Width, Height);
					if (Options.DeferredDirectionalDebugMode
						!= EDeferredDirectionalDebugMode::Disabled)
					{
						++PreparedView.Counters.DeferredDirectionalDebugViews;
					}
					if (DeferredTimingQuery)
						DeferredTimingSink(DeferredTimingQuery);
					const FDeferredDirectionalCaptureSink CaptureSink =
						GDeferredDirectionalCaptureSink.load(
							std::memory_order_acquire
						);
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
					++PreparedView.Counters.DeferredDirectionalPassFailures;
				}
			}
		}

		const bool bProductionResourcesReady =
			!bWantsProductionDeferred
			|| (bGBufferComplete && bHybridRetainedResourcesReady);
		if (bWantsProductionDeferred)
			DeferredParameters.DiagnosticMode = 0;
		FGPUTimingQueryRHIRef SceneColorTimingQuery;
		const FSceneColorTimingQuerySink SceneColorTimingSink =
			GSceneColorTimingQuerySink.load(std::memory_order_acquire);
		if (SceneColorTimingSink != nullptr && GDynamicRHI != nullptr)
		{
			SceneColorTimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (SceneColorTimingQuery)
				CommandList.BeginGPUTimingQuery(SceneColorTimingQuery);
		}
		const ERenderViewResult SceneResult = RenderScene_RenderThread(
			CommandList, PreparedView, SceneColor,
			SceneTargets->DirectionalDirect, SceneTargets->Depth,
			bWantsProductionDeferred && bProductionResourcesReady ? &DeferredParameters : nullptr
		);
		if (SceneColorTimingQuery)
		{
			CommandList.EndGPUTimingQuery(SceneColorTimingQuery);
			SceneColorTimingSink(SceneColorTimingQuery);
		}
		if (SceneResult != ERenderViewResult::Success)
			return SceneResult;
		CopyStaticMeshCounters(
			PreparedView.StaticMeshes, PreparedView.Counters
		);
		CopySkeletalMeshCounters(
			PreparedView.SkeletalMeshes, PreparedView.Counters
		);
		CopyTerrainCounters(PreparedView.Terrains, PreparedView.Counters);

		FRHITexture* PostProcessInput = SceneColor;
		if (Options.GBufferDebugMode != EGBufferDebugMode::Disabled
			&& GBufferTargets != nullptr
			&& SceneTargets->ContactColor != nullptr)
		{
			if (GBufferDebugRenderer.Render_RenderThread(
					CommandList,
					GBufferTargets->Material,
					GBufferTargets->Normals,
					GBufferTargets->Surface,
					GBufferTargets->Emissive,
					SceneTargets->Depth,
					SceneTargets->ContactColor,
					RenderView,
					Options.GBufferDebugMode,
					Width,
					Height
				))
			{
				PostProcessInput = SceneTargets->ContactColor;
				++PreparedView.Counters.GBufferDebugViews;
			}
			else
			{
				++PreparedView.Counters.GBufferDebugFailures;
			}
		}
		else if (GroundTruthAmbientOcclusionDebugOutput != nullptr)
		{
			PostProcessInput = GroundTruthAmbientOcclusionDebugOutput;
		}
		else if (RenderView.Settings.bEnableContactShadows
				 && PreparedView.DirectionalShadow.bEnabled
				 && SceneTargets->ContactColor != nullptr)
		{
			if (ContactShadowRenderer.Render_RenderThread(
					CommandList,
					SceneColor,
					SceneTargets->DirectionalDirect,
					SceneTargets->Depth,
					SceneTargets->ContactColor,
					RenderView,
					PreparedView.DirectionalShadow.LightDirection,
					RenderView.Settings.bShowContactShadowDebug,
					Width,
					Height
				))
			{
				PostProcessInput = SceneTargets->ContactColor;
				++PreparedView.Counters.ContactShadowEnabledViews;
			}
			else
			{
				++PreparedView.Counters.ContactShadowPassFailures;
			}
		}
		const FHDRSceneColorCaptureSink HDRCaptureSink =
			GHDRSceneColorCaptureSink.load(std::memory_order_acquire);
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
		FGPUTimingQueryRHIRef PostProcessTimingQuery;
		const FPostProcessTimingQuerySink PostProcessTimingSink =
			GPostProcessTimingQuerySink.load(std::memory_order_acquire);
		if (PostProcessTimingSink != nullptr && GDynamicRHI != nullptr)
		{
			PostProcessTimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (PostProcessTimingQuery)
				CommandList.BeginGPUTimingQuery(PostProcessTimingQuery);
		}
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
			View.Settings.bEnableFXAA,
			bHasEditorAssistance,
			RenderView.Settings.ExposureEV
		);
		CommandList.EndRenderPass();
		if (PostProcessTimingQuery)
		{
			CommandList.EndGPUTimingQuery(PostProcessTimingQuery);
			PostProcessTimingSink(PostProcessTimingQuery);
		}
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

	auto FSceneRenderer::RenderScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSceneView& PreparedView,
		FRHITexture* SceneColor,
		FRHITexture* DirectionalDirect,
		FRHITexture* Depth,
		const FDeferredDirectionalLightingRenderer::FRenderParameters*
			DeferredParameters
	) -> ERenderViewResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = PreparedView.View;
		if (SceneColor == nullptr || DirectionalDirect == nullptr || Depth == nullptr)
			return ERenderViewResult::RendererResourcesUnavailable;
		if (View.Settings.RenderMode != ERenderMode::Lit
			|| View.Settings.RasterMode != ERasterMode::Solid)
		{
			FRHIRenderPassInfo ScenePassInfo{};
			ScenePassInfo.RenderTargetLayout =
				RenderTargetLayouts::MakeSceneTargets();
			ScenePassInfo.ColorRenderTargets[0] = SceneColor;
			ScenePassInfo.ColorRenderTargets[1] = DirectionalDirect;
			ScenePassInfo.DepthStencilRenderTarget = Depth;
			ScenePassInfo.ColorClearValues[0] = FClearValueBinding(
				View.ClearColor.r, View.ClearColor.g,
				View.ClearColor.b, View.ClearColor.a
			);
			ScenePassInfo.ColorClearValues[1] =
				FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f);
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
			++PreparedView.Counters.HybridDeferredUnavailableViews;
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
		Bootstrap.ColorRenderTargets[1] = DirectionalDirect;
		Bootstrap.DepthStencilRenderTarget = Depth;
		Bootstrap.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r, View.ClearColor.g,
			View.ClearColor.b, View.ClearColor.a
		);
		Bootstrap.ColorClearValues[1] =
			FClearValueBinding(0.0f, 0.0f, 0.0f, 0.0f);
		CommandList.BeginRenderPass(Bootstrap, "HybridSceneBootstrapRenderPass");
		SetViewRect();
		bool bBootstrapRendered = true;
		if (PreparedView.bHasSkyBox)
		{
			if (PreparedView.bHasViewEnvironment)
			{
				bBootstrapRendered = SkyBoxRenderer.DrawTexture_RenderThread(
					CommandList, View, PreparedView.ViewEnvironmentTexture,
					PreparedView.SkyBox, true
				);
			}
			else
			{
				SkyBoxRenderer.Draw_RenderThread(
					CommandList, View, PreparedView.SkyBox, true
				);
			}
		}
		CommandList.EndRenderPass();
		if (!bBootstrapRendered)
			return ERenderViewResult::RequiredEnvironmentUnavailable;

		FGPUTimingQueryRHIRef DeferredTimingQuery;
		const FDeferredDirectionalTimingQuerySink DeferredTimingSink =
			GDeferredDirectionalTimingQuerySink.load(std::memory_order_acquire);
		if (DeferredTimingSink != nullptr && GDynamicRHI != nullptr)
		{
			DeferredTimingQuery = GDynamicRHI->RHICreateGPUTimingQuery();
			if (DeferredTimingQuery)
				CommandList.BeginGPUTimingQuery(DeferredTimingQuery);
		}
		const bool bDeferredRendered =
			DeferredDirectionalLightingRenderer.RenderProduction_RenderThread(
				CommandList, SceneColor, DirectionalDirect, *DeferredParameters
			);
		if (DeferredTimingQuery)
		{
			CommandList.EndGPUTimingQuery(DeferredTimingQuery);
			DeferredTimingSink(DeferredTimingQuery);
		}
		if (!bDeferredRendered)
		{
			++PreparedView.Counters.HybridDeferredUnavailableViews;
			return ERenderViewResult::RendererResourcesUnavailable;
		}

		FRHIRenderPassInfo Retained{};
		Retained.RenderTargetLayout =
			RenderTargetLayouts::MakeHybridRetainedForward();
		Retained.ColorRenderTargets[0] = SceneColor;
		Retained.ColorRenderTargets[1] = DirectionalDirect;
		Retained.DepthStencilRenderTarget = Depth;
		CommandList.BeginRenderPass(Retained, "HybridRetainedForwardRenderPass");
		SetViewRect();
		for (const EStaticMeshBasePass Pass : {
				 EStaticMeshBasePass::Opaque, EStaticMeshBasePass::Masked
			 })
		{
			const auto& StaticDraws = Pass == EStaticMeshBasePass::Opaque ? PreparedView.StaticMeshes.Opaque : PreparedView.StaticMeshes.Masked;
			for (const FPreparedStaticMeshDraw& Draw : StaticDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, PreparedView.LightingUniformBuffer,
						View.Settings.RenderMode, Pass, Draw,
						PreparedView.StaticMeshes, true
					);
				}
			const auto& SkeletalDraws = Pass == EStaticMeshBasePass::Opaque ? PreparedView.SkeletalMeshes.Opaque : PreparedView.SkeletalMeshes.Masked;
			for (const FPreparedSkeletalMeshDraw& Draw : SkeletalDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, PreparedView.LightingUniformBuffer,
						View.Settings.RenderMode, Pass, Draw,
						PreparedView.SkeletalMeshes, true
					);
				}
			const auto& TerrainDraws = Pass == EStaticMeshBasePass::Opaque ? PreparedView.Terrains.Opaque : PreparedView.Terrains.Masked;
			for (const FPreparedTerrainDraw& Draw : TerrainDraws)
				if (Draw.Material.PipelineIdentity.ShaderMap.ShadingModel
					!= EMaterialShadingModel::Lit)
				{
					TerrainRenderer.ExecutePreparedDraw_RenderThread(
						CommandList, View, PreparedView.LightingUniformBuffer,
						View.Settings.RenderMode, Draw, PreparedView.Terrains,
						true
					);
				}
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 PreparedView.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.StaticMeshes, true
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.SkeletalMeshes, true
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode,
					PreparedView.Terrains.Translucent[Draw.DrawIndex],
					PreparedView.Terrains, true
				);
		}
		CommandList.EndRenderPass();
		// Lit opaque/masked sections were already consumed by GBuffer + deferred
		// lighting, so the retained-forward attempted count intentionally does not
		// equal every prepared section as it does in the all-forward finalizer.
		PreparedView.StaticMeshes.Phase = EPreparedStaticMeshPhase::Executed;
		PreparedView.SkeletalMeshes.Phase = EPreparedSkeletalMeshPhase::Executed;
		PreparedView.Terrains.Phase = EPreparedTerrainPhase::Executed;
		++PreparedView.Counters.HybridDeferredEnabledViews;
		return ERenderViewResult::Success;
	}

	auto FSceneRenderer::RenderSpecialForwardScene_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSceneView& PreparedView,
		FRHITexture* RenderTarget
	) -> bool
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
			1.0f
		);
		CommandList.SetScissor(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(Width),
			static_cast<float>(Height)
		);

		if (PreparedView.bHasSkyBox)
		{
			if (PreparedView.bHasViewEnvironment)
			{
				if (!SkyBoxRenderer.DrawTexture_RenderThread(
						CommandList,
						View,
						PreparedView.ViewEnvironmentTexture,
						PreparedView.SkyBox
					))
				{
					return false;
				}
			}
			else
			{
				SkyBoxRenderer.Draw_RenderThread(
					CommandList, View, PreparedView.SkyBox
				);
			}
		}

		for (const EStaticMeshBasePass Pass : {
				 EStaticMeshBasePass::Opaque, EStaticMeshBasePass::Masked
			 })
		{
			StaticMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.LightingUniformBuffer,
				View.Settings.RenderMode, Pass, PreparedView.StaticMeshes
			);
			SkeletalMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.LightingUniformBuffer,
				View.Settings.RenderMode, Pass, PreparedView.SkeletalMeshes
			);
			TerrainRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.LightingUniformBuffer,
				View.Settings.RenderMode, Pass, PreparedView.Terrains
			);
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 PreparedView.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.StaticMeshes
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.SkeletalMeshes
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.RenderMode,
					PreparedView.Terrains.Translucent[Draw.DrawIndex],
					PreparedView.Terrains
				);
		}
		StaticMeshRenderer.FinalizeExecution_RenderThread(
			PreparedView.StaticMeshes
		);
		SkeletalMeshRenderer.FinalizeExecution_RenderThread(
			PreparedView.SkeletalMeshes
		);
		TerrainRenderer.FinalizeExecution_RenderThread(PreparedView.Terrains);
		return true;
	}
} // namespace Durin
