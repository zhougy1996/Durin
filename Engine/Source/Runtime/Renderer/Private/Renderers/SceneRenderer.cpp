#include "Renderers/SceneRenderer.h"

#include "Engine/TerrainSceneProxy.h"
#include "Renderers/PreparedSceneView.h"
#include "Renderers/ForwardLighting.h"
#include "Renderers/DirectionalShadowView.h"
#include "Renderers/TerrainRenderPreparation.h"
#include "Renderers/SceneRendererProfiling.h"
#include "Renderers/SceneViewState.h"
#include "Renderers/VolumetricCloudScenePreparation.h"

#include "Profiling/Profiling.h"

#include "Asset.h"
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
		auto ReportRejectedViewState(
			std::string_view Reason,
			FSceneViewStateId Id
		) -> void
		{
			static uint32 DiagnosticCount = 0;
			if (DiagnosticCount >= 16)
				return;
			++DiagnosticCount;
			DURIN_WARN(
				"Renderer rejected {} view-state identity {}.",
				Reason, FSceneViewStateIdAccess::GetValue(Id)
			);
		}

		class FViewStateSubmissionScope final
		{
		public:
			explicit FViewStateSubmissionScope(FSceneViewState* InState)
				: State(InState)
			{
			}

			~FViewStateSubmissionScope()
			{
				if (State != nullptr)
					State->Abort();
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

		std::atomic<FSceneColorTimingQuerySink> GSceneColorTimingQuerySink = nullptr;
		std::atomic<FPostProcessTimingQuerySink> GPostProcessTimingQuerySink = nullptr;
		std::atomic<FGBufferTimingQuerySink> GGBufferTimingQuerySink = nullptr;
		std::atomic<FDeferredDirectionalTimingQuerySink>
			GDeferredDirectionalTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionTimingQuerySink>
			GGroundTruthAmbientOcclusionTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionFilterTimingQuerySink>
			GGroundTruthAmbientOcclusionFilterTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionResolveTimingQuerySink>
			GGroundTruthAmbientOcclusionResolveTimingQuerySink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionFeatureTimingQuerySink>
			GGroundTruthAmbientOcclusionFeatureTimingQuerySink = nullptr;
		std::atomic<FHDRSceneColorCaptureSink> GHDRSceneColorCaptureSink = nullptr;
		std::atomic<FGBufferCaptureSink> GGBufferCaptureSink = nullptr;
		std::atomic<FDeferredDirectionalCaptureSink>
			GDeferredDirectionalCaptureSink = nullptr;
		std::atomic<FGroundTruthAmbientOcclusionCaptureSink>
			GGroundTruthAmbientOcclusionCaptureSink = nullptr;
		std::atomic<FVolumetricCloudPreparationSink>
			GVolumetricCloudPreparationSink = nullptr;

		template<typename TimingQuerySink>
		class TScopedGPUTimingQuery final
		{
		public:
			TScopedGPUTimingQuery(
				FRHICommandListImmediate& InCommandList,
				TimingQuerySink InSink
			)
				: CommandList(InCommandList)
				, Sink(InSink)
			{
				if (Sink == nullptr || GDynamicRHI == nullptr)
					return;
				Query = GDynamicRHI->RHICreateGPUTimingQuery();
				if (Query)
					CommandList.BeginGPUTimingQuery(Query);
			}

			~TScopedGPUTimingQuery()
			{
				End();
			}

			TScopedGPUTimingQuery(const TScopedGPUTimingQuery&) = delete;
			auto operator=(const TScopedGPUTimingQuery&)
				-> TScopedGPUTimingQuery& = delete;

			auto End() -> void
			{
				if (Query && !bEnded)
				{
					CommandList.EndGPUTimingQuery(Query);
					bEnded = true;
				}
			}

			auto Commit() -> void
			{
				if (!Query || bCommitted)
					return;
				End();
				Sink(Query);
				bCommitted = true;
			}

		private:
			FRHICommandListImmediate& CommandList;
			TimingQuerySink Sink;
			FGPUTimingQueryRHIRef Query;
			bool bEnded = false;
			bool bCommitted = false;
		};

		class FViewCounterSnapshotScope final
		{
		public:
			FViewCounterSnapshotScope(
				const FViewRenderCounters& InCounters,
				FSceneViewStatistics* InOutStatistics
			)
				: Counters(InCounters)
				, OutStatistics(InOutStatistics)
			{
			}

			~FViewCounterSnapshotScope()
			{
				EmitViewRenderCounterSnapshot(Counters);
				if (OutStatistics != nullptr)
					*OutStatistics = BuildSceneViewStatistics(Counters);
			}

		private:
			const FViewRenderCounters& Counters;
			FSceneViewStatistics* OutStatistics = nullptr;
		};

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
			const FPreparedSkeletalPaletteTable& Palettes,
			FViewRenderCounters& Counters
		) -> void
		{
			check(Palettes.RequestedPalettes == Palettes.UploadedPalettes + Palettes.ReusedPalettes + Palettes.RejectedPalettes);
			check(Palettes.UploadedBytes == Palettes.UploadedMatrices * sizeof(FMatrix4f));
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
			Counters.RequestedSkeletalPaletteUploads = Palettes.RequestedPalettes;
			Counters.UploadedSkeletalPalettes = Palettes.UploadedPalettes;
			Counters.ReusedSkeletalPalettes = Palettes.ReusedPalettes;
			Counters.RejectedSkeletalPalettes = Palettes.RejectedPalettes;
			Counters.UploadedSkeletalPaletteMatrices = Palettes.UploadedMatrices;
			Counters.UploadedSkeletalPaletteBytes = Palettes.UploadedBytes;
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
		Result.Visibility.SubmittedPrimitives = Counters.SubmittedPrimitives;
		Result.Visibility.VisiblePrimitives = Counters.VisiblePrimitives;
		Result.StaticMesh.Primitives = Counters.PreparedStaticMeshPrimitives;
		Result.SplineMesh.Primitives = Counters.PreparedSplineMeshPrimitives;
		Result.SkeletalMesh.Primitives = Counters.PreparedSkeletalMeshPrimitives;
		Result.Terrain.VisiblePatches = Counters.VisibleTerrainPatches;

		Result.SplineMesh.Triangles = Counters.PreparedSplineMeshTriangles;
		Result.StaticMesh.Triangles = Counters.PreparedStaticMeshTriangles
									 - std::min(Counters.PreparedStaticMeshTriangles, Counters.PreparedSplineMeshTriangles);
		Result.SkeletalMesh.Triangles = Counters.PreparedSkeletalMeshTriangles;
		Result.Terrain.Triangles = Counters.PreparedTerrainTriangles;
		Result.Summary.Triangles = AddSaturated(
			AddSaturated(Result.StaticMesh.Triangles, Result.SplineMesh.Triangles),
			AddSaturated(Result.SkeletalMesh.Triangles, Result.Terrain.Triangles)
		);
		Result.Shadow.Triangles = Counters.ShadowPreparedTriangles;

		Result.StaticMesh.DrawCalls = Counters.StaticMeshSuccessfulDraws;
		Result.SkeletalMesh.DrawCalls = Counters.SkeletalMeshSuccessfulDraws;
		Result.Terrain.DrawCalls = Counters.TerrainSuccessfulDraws;
		Result.Shadow.DrawCalls = Counters.ShadowSuccessfulDraws;
		Result.Lights.Directional = Counters.SelectedDirectionalLights;
		Result.Lights.Point = Counters.SelectedPointLights;
		Result.Lights.Spot = Counters.SelectedSpotLights;
		Result.Shadow.Cascades = static_cast<uint32>(std::min<size_t>(
			Counters.ShadowCascadeCount, std::numeric_limits<uint32>::max()
		));
		Result.Shadow.bEnabled = Counters.ShadowValidReceiverViews != 0
								&& Counters.ShadowCascadeCount != 0;
		Result.Shadow.bContactEnabled = Counters.ContactShadowEnabledViews != 0;
		if (Counters.ContactShadowComputeViews != 0)
			Result.Shadow.ContactRoute = EContactShadowExecutionRoute::Compute;
		else if (Counters.ContactShadowFragmentViews != 0)
			Result.Shadow.ContactRoute = EContactShadowExecutionRoute::Fragment;
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

	auto SetGroundTruthAmbientOcclusionResolveTimingQuerySink(
		FGroundTruthAmbientOcclusionResolveTimingQuerySink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionResolveTimingQuerySink.store(
			Sink, std::memory_order_release
		);
	}

	auto SetGroundTruthAmbientOcclusionFeatureTimingQuerySink(
		FGroundTruthAmbientOcclusionFeatureTimingQuerySink Sink
	) -> void
	{
		GGroundTruthAmbientOcclusionFeatureTimingQuerySink.store(
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

	auto SetVolumetricCloudPreparationSink(
		FVolumetricCloudPreparationSink Sink
	) -> void
	{
		GVolumetricCloudPreparationSink.store(Sink, std::memory_order_release);
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
		, VolumetricCloudRenderer(Coordinator, FullscreenGeometry)
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
		VolumetricCloudRenderer.ReleaseResources_RenderThread();
		FullscreenGeometry.ReleaseResources_RenderThread();
	}

	auto FSceneRenderer::AddViewState_RenderThread(FSceneViewStateId Id) -> bool
	{
		return ViewStates.Add(Id);
	}

	auto FSceneRenderer::RemoveViewState_RenderThread(FSceneViewStateId Id) -> bool
	{
		return ViewStates.Remove(Id);
	}

	auto FSceneRenderer::InvalidateViewState_RenderThread(
		FSceneViewStateId Id
	) -> bool
	{
		return ViewStates.Invalidate(
			Id, ESceneViewDiscontinuity::ManualInvalidation
		);
	}

	auto FSceneRenderer::InvalidateAllViewStates_RenderThread() -> void
	{
		ViewStates.InvalidateAll(
			ESceneViewDiscontinuity::ManualInvalidation
		);
	}

	auto FSceneRenderer::ReleaseViewStates_RenderThread() -> size_t
	{
		return ViewStates.ReleaseAll();
	}

	auto FSceneRenderer::GetViewStateCount_RenderThread() const -> size_t
	{
		check(IsInRenderingThread());
		return ViewStates.Num();
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
		if (Cause == ERendererResourceInvalidationCause::Device)
			ViewStates.InvalidateAll(
				ESceneViewDiscontinuity::DeviceInvalidation
			);
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
						VolumetricCloudRenderer.ReleaseResources_RenderThread();
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

	auto FSceneRenderer::PrepareView_RenderThread(
		FRHICommandListImmediate& CommandList,
		FScene* Scene,
		FSceneView& RenderView,
		const FSceneViewRenderOptions& Options,
		FPreparedSceneView& PreparedView
	) -> ERenderViewResult
	{
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
				const auto ShadowPreparationStart =
					std::chrono::steady_clock::now();
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
					const auto DiscoveryStart = std::chrono::steady_clock::now();
					PreparedView.DirectionalShadowCasters =
						PrepareDirectionalShadowCasterTable(
							*Scene, PreparedView.DirectionalShadow
						);
					PreparedView.Counters.ShadowDiscoveryMembershipNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now() - DiscoveryStart
						)
												.count());
					const auto& CasterTable = PreparedView.DirectionalShadowCasters;
					PreparedView.Counters.ShadowSceneTraversals =
						CasterTable.SceneTraversals;
					PreparedView.Counters.ShadowUniqueSubmittedCasters =
						CasterTable.UniqueSubmitted;
					PreparedView.Counters.ShadowUniqueHiddenCasters =
						CasterTable.UniqueHidden;
					PreparedView.Counters.ShadowUniqueEligibleStaticMeshCasters =
						CasterTable.UniqueEligibleStaticMeshes;
					PreparedView.Counters.ShadowUniqueEligibleSplineMeshCasters =
						CasterTable.UniqueEligibleSplineMeshes;
					PreparedView.Counters.ShadowUniqueEligibleSkeletalMeshCasters =
						CasterTable.UniqueEligibleSkeletalMeshes;
					PreparedView.Counters.ShadowUniqueEligibleTerrainCasters =
						CasterTable.UniqueEligibleTerrains;
					PreparedView.Counters.ShadowCascadeClassificationTests =
						CasterTable.CascadeClassificationTests;
					PreparedView.Counters.ShadowMembershipPopcount =
						CasterTable.MembershipPopcount;
					PreparedView.Counters.ShadowTemporaryBytes =
						CasterTable.TemporaryBytes;
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
						const FDirectionalShadowCasterCandidates& Casters =
							CasterTable.Cascades[CascadeIndex];
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
						const auto StaticSplineStart =
							std::chrono::steady_clock::now();
						StaticMeshes = PrepareStaticMeshView_RenderThread(
							CommandList, Casters.StaticMeshes, Cascade.CasterView,
							ERasterMode::Solid, Casters.SplineMeshes,
							ERenderPreparationMode::ShadowDepth
						);
						PreparedView.Counters.ShadowStaticSplinePreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now()
													- StaticSplineStart
							)
													.count());
						const auto SkeletalStart = std::chrono::steady_clock::now();
						SkeletalMeshes = PrepareSkeletalMeshView_RenderThread(
							CommandList, Casters.SkeletalMeshes, Cascade.CasterView,
							ERasterMode::Solid, PreparedView.SkeletalPalettes,
							ERenderPreparationMode::ShadowDepth
						);
						PreparedView.Counters.ShadowSkeletalPreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now() - SkeletalStart
							)
													.count());
						const auto TerrainStart = std::chrono::steady_clock::now();
						Terrains = PrepareTerrainView_RenderThread(
							Casters.Terrains, Cascade.CasterView, ERasterMode::Solid,
							ERenderPreparationMode::ShadowDepth
						);
						PreparedView.Counters.ShadowTerrainLogicalPreparationNanoseconds +=
							static_cast<uint64>(std::chrono::duration_cast<
													std::chrono::nanoseconds>(
													std::chrono::steady_clock::now() - TerrainStart
							)
													.count());
						PreparedView.Counters.ShadowSortingBatchingNanoseconds +=
							StaticMeshes.SortingNanoseconds
							+ SkeletalMeshes.SortingNanoseconds
							+ Terrains.BatchConstructionNanoseconds;
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
					PreparedView.Counters.ShadowLogicalPreparationNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now()
												- ShadowPreparationStart
						)
												.count());
				}
				else if (Selected.Data.bCastShadows)
				{
					++PreparedView.Counters.ShadowInvalidReceiverViews;
					PreparedView.Counters.ShadowLogicalPreparationNanoseconds =
						static_cast<uint64>(std::chrono::duration_cast<
												std::chrono::nanoseconds>(
												std::chrono::steady_clock::now()
												- ShadowPreparationStart
						)
												.count());
				}
			}
			PreparedView.StaticMeshes = PrepareStaticMeshView_RenderThread(
				CommandList,
				Visibility.StaticMeshSceneInfos,
				RenderView,
				RenderView.Settings.Mode.RasterMode,
				Visibility.SplineMeshSceneInfos
			);
			PreparedView.SkeletalMeshes = PrepareSkeletalMeshView_RenderThread(
				CommandList, Visibility.SkeletalMeshSceneInfos, RenderView,
				RenderView.Settings.Mode.RasterMode,
				PreparedView.SkeletalPalettes
			);
			PreparedView.Terrains = PrepareTerrainView_RenderThread(
				Visibility.TerrainSceneInfos, RenderView,
				RenderView.Settings.Mode.RasterMode
			);
			if (RenderView.Settings.Terrain.bShowLODOverlay)
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
			RenderView.Settings.Mode.RenderMode == ERenderMode::Lit
			&& RenderView.Settings.Mode.RasterMode == ERasterMode::Solid;
		StaticMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.StaticMeshes, !bRequiresDeferredOpaque
		);
		SkeletalMeshRenderer.PrepareResources_RenderThread(
			CommandList, PreparedView.SkeletalPalettes,
			PreparedView.SkeletalMeshes, !bRequiresDeferredOpaque
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
			PreparedView.SkeletalMeshes, PreparedView.SkeletalPalettes,
			PreparedView.Counters
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
		if (Scene != nullptr)
		{
			FVolumetricCloudSceneData Cloud;
			if (Scene->GetActiveVolumetricCloud_RenderThread(Cloud))
			{
				PreparedView.bVolumetricCloudRequested = true;
				PreparedView.VolumetricCloudHistoryKey = GetTypeHash(Cloud.PersistentId)
														 ^ (Cloud.InstanceId + 0x9e3779b97f4a7c15ull
															+ (Cloud.PublicationRevision << 6)
															+ (Cloud.PublicationRevision >> 2));
				PreparedView.VolumetricCloudParameters =
					BuildVolumetricCloudParameters(Cloud, PreparedView.Lights);
				auto ResolveDimension = [](const FRHITextureReferenceRef& Reference,
										   ETextureDimension Dimension) -> FRHITexture* {
					FRHITexture* Texture = Reference != nullptr ? Reference->GetReferencedTexture_RenderThread() : nullptr;
					return Texture != nullptr && Texture->GetDimension() == Dimension ? Texture : nullptr;
				};
				PreparedView.VolumetricCloudTextures.BaseDensity = ResolveDimension(
					Cloud.BaseDensityTexture, ETextureDimension::Texture3D
				);
				PreparedView.VolumetricCloudTextures.DetailDensity = ResolveDimension(
					Cloud.DetailDensityTexture, ETextureDimension::Texture3D
				);
				PreparedView.VolumetricCloudTextures.Weather = ResolveDimension(
					Cloud.WeatherTexture, ETextureDimension::Texture2D
				);
				PreparedView.VolumetricCloudTextures.DensitySampler =
					VolumetricCloudRenderer.EnsureDensitySampler_RenderThread();
			}
		}
		const FVolumetricCloudPreparationSink CloudPreparationSink =
			GVolumetricCloudPreparationSink.load(std::memory_order_acquire);
		if (CloudPreparationSink != nullptr)
			CloudPreparationSink(PreparedView);

		return ERenderViewResult::Success;
	}

	auto FSceneRenderer::RenderGBuffer_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSceneView& PreparedView,
		FPostProcessRenderer::FSceneTargets* SceneTargets,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bNeedsGBuffer,
		bool bWantsIsolatedDeferred
	) -> FGBufferRenderer::FTargets*
	{
		const FSceneView& RenderView = PreparedView.View;
		FGBufferRenderer::FTargets* GBufferTargets = nullptr;
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
					RenderView.DepthConvention == ESceneDepthConvention::ReversedZ ? 0.0f : 1.0f,
					0u
				);
				const FGBufferTimingQuerySink GBufferTimingSink =
					GGBufferTimingQuerySink.load(std::memory_order_acquire);
				TScopedGPUTimingQuery GBufferTiming(
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
				GBufferTiming.Commit();
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
		return GBufferTargets;
	}

	auto FSceneRenderer::RenderGroundTruthAmbientOcclusion_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSceneView& PreparedView,
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
		const FSceneView& RenderView = PreparedView.View;
		if (bWantsGroundTruthAmbientOcclusion)
		{
			++PreparedView.Counters.GroundTruthAmbientOcclusionAttemptedViews;
			auto* AmbientOcclusionTargets = bGBufferComplete ? GroundTruthAmbientOcclusionRenderer.EnsureTargets_RenderThread(
														   Width, Height,
														   RenderView.Settings.AmbientOcclusion.Quality
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
				const FGroundTruthAmbientOcclusionFeatureTimingQuerySink
					FeatureTimingSink =
						GGroundTruthAmbientOcclusionFeatureTimingQuerySink.load(
							std::memory_order_acquire
						);
				TScopedGPUTimingQuery FeatureTiming(
					CommandList, FeatureTimingSink
				);
				const FGroundTruthAmbientOcclusionTimingQuerySink TimingSink =
					GGroundTruthAmbientOcclusionTimingQuerySink.load(
						std::memory_order_acquire
					);
				bool bRendered = false;
				{
					TScopedGPUTimingQuery RawTiming(CommandList, TimingSink);
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
						GGroundTruthAmbientOcclusionCaptureSink.load(
							std::memory_order_acquire
						);
					if (CaptureSink != nullptr)
						CaptureSink(
							CommandList, AmbientOcclusionTargets->Raw, false
						);

					const FGroundTruthAmbientOcclusionFilterTimingQuerySink
						FilterTimingSink =
							GGroundTruthAmbientOcclusionFilterTimingQuerySink.load(
								std::memory_order_acquire
							);
					TScopedGPUTimingQuery FilterTiming(
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
							GGroundTruthAmbientOcclusionResolveTimingQuerySink.load(
								std::memory_order_acquire
							);
					bool bResolved = false;
					if (bFiltered)
					{
						TScopedGPUTimingQuery ResolveTiming(
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
						++PreparedView.Counters.GroundTruthAmbientOcclusionEnabledViews;
						if (AmbientOcclusionTargets->Quality
							== EGroundTruthAmbientOcclusionQuality::HalfResolution)
							++PreparedView.Counters.GroundTruthAmbientOcclusionHalfResolutionViews;
						else
							++PreparedView.Counters.GroundTruthAmbientOcclusionFullResolutionViews;
						PreparedView.Counters.GroundTruthAmbientOcclusionActiveBytes =
							FGroundTruthAmbientOcclusionRenderer::
								CalculateTargetBytes(Width, Height, AmbientOcclusionTargets->Quality);
						if (Options.GroundTruthAmbientOcclusionDebugMode
							!= EGroundTruthAmbientOcclusionDebugMode::Disabled)
						{
							++PreparedView.Counters.GroundTruthAmbientOcclusionDebugViews;
						}
						FilterTiming.Commit();
						if (CaptureSink != nullptr)
							CaptureSink(
								CommandList, AmbientOcclusionTargets->Raw, true
							);
					}
					else if (!bFiltered)
					{
						++PreparedView.Counters.GroundTruthAmbientOcclusionFilterPassFailures;
					}
					else
					{
						++PreparedView.Counters.GroundTruthAmbientOcclusionResolvePassFailures;
					}
				}
				else
				{
					++PreparedView.Counters.GroundTruthAmbientOcclusionRawPassFailures;
				}
			}
		}
	}

	auto FSceneRenderer::RenderContactShadows_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSceneView& PreparedView,
		FGBufferRenderer::FTargets* GBufferTargets,
		FPostProcessRenderer::FSceneTargets* SceneTargets,
		FDeferredDirectionalLightingRenderer::FRenderParameters& DeferredParameters,
		const FSceneViewRenderOptions& Options,
		uint32 Width,
		uint32 Height,
		bool bWantsProductionDeferred,
		bool bGBufferComplete
	) -> void
	{
		const FSceneView& RenderView = PreparedView.View;
		const bool bWantsContactVisibility = bWantsProductionDeferred
											 && RenderView.Settings.DirectionalShadow.bEnableContactShadows
											 && PreparedView.DirectionalShadow.bEnabled;
		if (bWantsContactVisibility && bGBufferComplete
			&& PreparedView.Counters.GBufferSuccessfulDraws != 0)
		{
			const EContactShadowRoutePreference RoutePreference =
				RenderView.Settings.DirectionalShadow.ContactRoutePreference;
			const bool bForceFragment = Options.bForceFragmentContactVisibility
										|| RoutePreference == EContactShadowRoutePreference::Fragment;
			const bool bForceCompute = !Options.bForceFragmentContactVisibility
									   && RoutePreference == EContactShadowRoutePreference::Compute;
			auto* FragmentContactTargets = bForceCompute ? nullptr : ContactShadowRenderer.EnsureTargets_RenderThread(Width, Height);
			auto* ComputeContactTargets = bForceFragment ? nullptr : ContactShadowRenderer.EnsureComputeTargets_RenderThread(Width, Height);
			PreparedView.Counters.ContactShadowRetainedBytes =
				ContactShadowRenderer.GetRetainedTargetBytes_RenderThread();
			const auto ContactResult = ContactShadowRenderer.Render_RenderThread(
				CommandList, true, FragmentContactTargets, ComputeContactTargets,
				GBufferTargets->Material, GBufferTargets->Normals,
				GBufferTargets->Surface, GBufferTargets->Emissive,
				SceneTargets->Depth, RenderView,
				PreparedView.DirectionalShadow.LightDirection, Width, Height
			);
			const size_t ReasonIndex = static_cast<size_t>(ContactResult.Reason);
			if (ReasonIndex < PreparedView.Counters.ContactShadowRouteReasons.size())
				++PreparedView.Counters.ContactShadowRouteReasons[ReasonIndex];
			if (ContactResult.Visibility != nullptr)
			{
				PreparedView.Counters.ContactShadowActiveBytes =
					FContactShadowVisibilityRenderer::CalculateTargetBytes(Width, Height);
				DeferredParameters.ContactVisibility = ContactResult.Visibility;
				DeferredParameters.bContactVisibilityEnabled = true;
				DeferredParameters.bContactVisibilityDebug =
					RenderView.Settings.DirectionalShadow.bShowContactDebug;
				++PreparedView.Counters.ContactShadowEnabledViews;
				if (ContactResult.Route
					== FContactShadowVisibilityRenderer::ERoute::Compute)
				{
					++PreparedView.Counters.ContactShadowComputeViews;
					++PreparedView.Counters.ContactShadowDispatches;
				}
				else
				{
					++PreparedView.Counters.ContactShadowFragmentViews;
					++PreparedView.Counters.ContactShadowDraws;
				}
			}
			else
			{
				++PreparedView.Counters.ContactShadowPassFailures;
				++PreparedView.Counters.ContactShadowFactorOneViews;
			}
		}
	}

	auto FSceneRenderer::RenderIsolatedDeferred_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSceneView& PreparedView,
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
				++PreparedView.Counters.DeferredDirectionalUnavailableViews;
			else
			{
				DeferredParameters.GroundTruthAmbientOcclusionDebugMode =
					static_cast<uint32>(
						Options.GroundTruthAmbientOcclusionDebugMode
					);
				const FDeferredDirectionalTimingQuerySink DeferredTimingSink =
					GDeferredDirectionalTimingQuerySink.load(
						std::memory_order_acquire
					);
				TScopedGPUTimingQuery DeferredTiming(
					CommandList, DeferredTimingSink
				);
				const bool bRendered =
					DeferredDirectionalLightingRenderer.Render_RenderThread(
						CommandList, *DeferredTargets, DeferredParameters
					);
				DeferredTiming.End();
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
					DeferredTiming.Commit();
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
		return GroundTruthAmbientOcclusionDebugOutput;
	}

	auto FSceneRenderer::RenderPostProcess_RenderThread(
		FRHICommandListImmediate& CommandList,
		FPreparedSceneView& PreparedView,
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
		const FSceneView& RenderView = PreparedView.View;
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
		const FPostProcessTimingQuerySink PostProcessTimingSink =
			GPostProcessTimingQuerySink.load(std::memory_order_acquire);
		TScopedGPUTimingQuery PostProcessTiming(
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
		if (RenderSubmissionSerial != std::numeric_limits<uint64>::max())
			++RenderSubmissionSerial;
		FPreparedSceneView PreparedView;
		FViewCounterSnapshotScope CounterSnapshotScope(
			PreparedView.Counters, OutStatistics
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
			PreparedView.TemporalContext.Current =
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				);
			PreparedView.TemporalContext.SubmissionSerial =
				RenderSubmissionSerial;
			PreparedView.TemporalContext.Discontinuities =
				ESceneViewDiscontinuity::DuplicateSubmission;
			ReportRejectedViewState(
				"an interleaved submission for",
				RenderView.ViewStateId
			);
			ViewState = nullptr;
		}
		FViewStateSubmissionScope ViewStateSubmission(ViewState);
		if (ViewState != nullptr)
		{
			PreparedView.TemporalContext = ViewState->Begin(
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				),
				RenderSubmissionSerial, RenderView.bDiscardHistory
			);
		}
		else if (PreparedView.TemporalContext.Discontinuities
				 != ESceneViewDiscontinuity::DuplicateSubmission)
		{
			PreparedView.TemporalContext.Current =
				BuildSceneViewTemporalMetadata(
					RenderView, Scene, Width, Height
				);
			PreparedView.TemporalContext.SubmissionSerial =
				RenderSubmissionSerial;
			PreparedView.TemporalContext.Discontinuities =
				ESceneViewDiscontinuity::MissingState;
			if (RenderView.ViewStateId.IsValid())
			{
				ReportRejectedViewState(
					"a missing, released, or foreign",
					RenderView.ViewStateId
				);
			}
		}
		PreparedView.ViewState = ViewState;
		const ERenderViewResult PreparationResult = PrepareView_RenderThread(
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
		GBufferTargets = RenderGBuffer_RenderThread(
			CommandList, PreparedView, SceneTargets, Options, Width, Height,
			bNeedsGBuffer, bWantsIsolatedDeferred
		);

		bool bGBufferComplete = false;
		FDeferredDirectionalLightingRenderer::FRenderParameters DeferredParameters;
		FRHITexture* GroundTruthAmbientOcclusionFallback =
			DefaultTextures.Get_RenderThread(EDefaultTexture::White);
		FRHITexture* ContactVisibilityFallback =
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
					.GroundTruthAmbientOcclusionResolved =
						GroundTruthAmbientOcclusionFallback,
					.GroundTruthAmbientOcclusionSelector =
						GroundTruthAmbientOcclusionFallback,
					.ContactVisibility = ContactVisibilityFallback,
					.Lighting = PreparedView.LightingUniformBuffer,
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
			bWantsProductionDeferred, bGBufferComplete
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
			GSceneColorTimingQuerySink.load(std::memory_order_acquire);
		TScopedGPUTimingQuery SceneColorTiming(
			CommandList, SceneColorTimingSink
		);
		const ERenderViewResult SceneResult = RenderScene_RenderThread(
			CommandList, PreparedView, SceneColor, SceneTargets->Depth,
			bWantsProductionDeferred && bProductionResourcesReady ? &DeferredParameters : nullptr
		);
		SceneColorTiming.Commit();
		if (SceneResult != ERenderViewResult::Success)
			return SceneResult;
		CopyStaticMeshCounters(
			PreparedView.StaticMeshes, PreparedView.Counters
		);
		CopySkeletalMeshCounters(
			PreparedView.SkeletalMeshes, PreparedView.SkeletalPalettes,
			PreparedView.Counters
		);
		CopyTerrainCounters(PreparedView.Terrains, PreparedView.Counters);

		const ERenderViewResult Result = RenderPostProcess_RenderThread(
			CommandList, PreparedView, View, OutputTarget, bPresentOutput,
			Options, SceneTargets, GBufferTargets, SceneColor,
			GroundTruthAmbientOcclusionDebugOutput
		);
		if (Result == ERenderViewResult::Success)
			ViewStateSubmission.Commit();
		return Result;
	}

	auto FSceneRenderer::RenderVolumetricCloud_RenderThread(
		FRHICommandListImmediate& CommandList, FPreparedSceneView& PreparedView, FRHITexture* SceneColor, FRHITexture* Depth
	) -> FRHITexture*
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = PreparedView.View;
		const uint32 Width = SceneColor != nullptr ? SceneColor->GetSizeX() : 0;
		const uint32 Height = SceneColor != nullptr ? SceneColor->GetSizeY() : 0;
		const bool bInputsPresent = PreparedView.VolumetricCloudTextures.BaseDensity != nullptr
									&& PreparedView.VolumetricCloudTextures.DetailDensity != nullptr
									&& PreparedView.VolumetricCloudTextures.DensitySampler != nullptr
									&& Depth != nullptr;
		constexpr auto QualityTier = FVolumetricCloudRenderer::EQualityTier::High;
		const auto Quality = FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(
			QualityTier
		);
		const auto CloudExtent = FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
			Width, Height, Quality
		);
		auto* FragmentTargets = PreparedView.bVolumetricCloudRequested && bInputsPresent ? VolumetricCloudRenderer.EnsureTargets_RenderThread(
																							   CloudExtent.Width, CloudExtent.Height
																						   ) :
																						   nullptr;
		auto* ComputeTargets = PreparedView.bVolumetricCloudRequested && bInputsPresent
									   && !PreparedView.bVolumetricCloudForceFragmentForQualification ?
								   VolumetricCloudRenderer.EnsureComputeTargets_RenderThread(
									   CloudExtent.Width, CloudExtent.Height
								   ) :
								   nullptr;
		auto Textures = PreparedView.VolumetricCloudTextures;
		Textures.SceneDepth = Depth;
		const FVolumetricCloudRenderer::FRenderResult Result =
			VolumetricCloudRenderer.Render_RenderThread(CommandList, FragmentTargets, ComputeTargets, {.bRequested = PreparedView.bVolumetricCloudRequested, .Textures = Textures, .Parameters = PreparedView.VolumetricCloudParameters, .View = &View, .QualityTier = QualityTier, .SuccessfulSequence = PreparedView.TemporalContext.SuccessfulSequence, .Width = CloudExtent.Width, .Height = CloudExtent.Height, .OutputWidth = Width, .OutputHeight = Height});
		auto& Counters = PreparedView.Counters;
		const auto RouteIndex = static_cast<size_t>(Result.Counters.Reason);
		if (RouteIndex < Counters.VolumetricCloudRouteReasons.size())
			++Counters.VolumetricCloudRouteReasons[RouteIndex];
		Counters.VolumetricCloudDispatches += Result.Counters.Dispatches;
		Counters.VolumetricCloudDraws += Result.Counters.Draws;
		Counters.VolumetricCloudPrimarySamples += Result.Counters.PrimarySamples;
		Counters.VolumetricCloudLightSamples += Result.Counters.LightSamples;
		Counters.VolumetricCloudActiveBytes = Result.Counters.TargetBytes;
		if (Result.Counters.Route == FVolumetricCloudRenderer::ERoute::Compute)
			++Counters.VolumetricCloudComputeViews;
		else if (Result.Counters.Route == FVolumetricCloudRenderer::ERoute::Fragment)
			++Counters.VolumetricCloudFragmentViews;
		else
			++Counters.VolumetricCloudDisabledViews;
		const FVolumetricCloudRenderer::FTemporalReconstructionResult Temporal =
			Result.Cloud != nullptr ? VolumetricCloudRenderer.ReconstructTemporal_RenderThread(
										  CommandList, {.CurrentCloud = Result.Cloud, .View = &View, .TemporalContext = &PreparedView.TemporalContext, .ViewState = PreparedView.ViewState, .Parameters = PreparedView.VolumetricCloudParameters, .QualityTier = QualityTier, .CloudHistoryKey = PreparedView.VolumetricCloudHistoryKey}
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
																 CommandList, SceneColor, Temporal.Cloud, Depth, View
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
		FPreparedSceneView& PreparedView,
		FRHITexture*& SceneColor,
		FRHITexture* Depth,
		const FDeferredDirectionalLightingRenderer::FRenderParameters*
			DeferredParameters
	) -> ERenderViewResult
	{
		check(IsInRenderingThread());
		check(!CommandList.IsInsideRenderPass());
		const FSceneView& View = PreparedView.View;
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
		Bootstrap.DepthStencilRenderTarget = Depth;
		Bootstrap.ColorClearValues[0] = FClearValueBinding(
			View.ClearColor.r, View.ClearColor.g,
			View.ClearColor.b, View.ClearColor.a
		);
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

		const FDeferredDirectionalTimingQuerySink DeferredTimingSink =
			GDeferredDirectionalTimingQuerySink.load(std::memory_order_acquire);
		TScopedGPUTimingQuery DeferredTiming(
			CommandList, DeferredTimingSink
		);
		const bool bDeferredRendered =
			DeferredDirectionalLightingRenderer.RenderProduction_RenderThread(
				CommandList, SceneColor, *DeferredParameters
			);
		DeferredTiming.Commit();
		if (!bDeferredRendered)
		{
			++PreparedView.Counters.HybridDeferredUnavailableViews;
			return ERenderViewResult::RendererResourcesUnavailable;
		}

		FRHIRenderPassInfo RetainedOpaque{};
		RetainedOpaque.RenderTargetLayout =
			RenderTargetLayouts::MakeHybridRetainedForward();
		RetainedOpaque.ColorRenderTargets[0] = SceneColor;
		RetainedOpaque.DepthStencilRenderTarget = Depth;
		CommandList.BeginRenderPass(
			RetainedOpaque, "HybridRetainedOpaqueRenderPass"
		);
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
						View.Settings.Mode.RenderMode, Pass, Draw,
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
						View.Settings.Mode.RenderMode, Pass, Draw,
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
						View.Settings.Mode.RenderMode, Draw, PreparedView.Terrains,
						true
					);
				}
		}
		CommandList.EndRenderPass();
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
			 PreparedView.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.Mode.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.StaticMeshes, true
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.Mode.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.SkeletalMeshes, true
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.Mode.RenderMode,
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
				View.Settings.Mode.RenderMode, Pass, PreparedView.StaticMeshes
			);
			SkeletalMeshRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.LightingUniformBuffer,
				View.Settings.Mode.RenderMode, Pass, PreparedView.SkeletalMeshes
			);
			TerrainRenderer.ExecutePass_RenderThread(
				CommandList, View, PreparedView.LightingUniformBuffer,
				View.Settings.Mode.RenderMode, Pass, PreparedView.Terrains
			);
		}
		for (const FPreparedTranslucentSceneDraw& Draw :
			 PreparedView.TranslucentGeometry)
		{
			if (Draw.Family == EPreparedTranslucentGeometryFamily::StaticMesh)
				StaticMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.Mode.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.StaticMeshes.Translucent[Draw.DrawIndex],
					PreparedView.StaticMeshes
				);
			else if (Draw.Family == EPreparedTranslucentGeometryFamily::SkeletalMesh)
				SkeletalMeshRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.Mode.RenderMode, EStaticMeshBasePass::Translucent,
					PreparedView.SkeletalMeshes.Translucent[Draw.DrawIndex],
					PreparedView.SkeletalMeshes
				);
			else
				TerrainRenderer.ExecutePreparedDraw_RenderThread(
					CommandList, View, PreparedView.LightingUniformBuffer,
					View.Settings.Mode.RenderMode,
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
