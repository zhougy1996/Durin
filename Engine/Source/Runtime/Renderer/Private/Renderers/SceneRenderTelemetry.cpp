#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SkeletalMeshRenderPreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/TerrainRenderPreparation.h"

namespace Durin
{
	namespace
	{
		std::atomic<FViewRenderCounterSink> GViewRenderCounterSink = nullptr;

		auto AddSaturated(uint64 A, uint64 B) -> uint64
		{
			return B > std::numeric_limits<uint64>::max() - A
				? std::numeric_limits<uint64>::max() : A + B;
		}
	}

	auto SetViewRenderCounterSink(FViewRenderCounterSink Sink) -> void
	{
		GViewRenderCounterSink.store(Sink, std::memory_order_release);
	}

	auto EmitViewRenderCounterSnapshot(
		const FViewRenderCounters& Counters) -> void
	{
		if (const FViewRenderCounterSink Sink =
			GViewRenderCounterSink.load(std::memory_order_acquire))
		{
			Sink(Counters);
		}
	}

	auto BuildSceneViewStatistics(const FViewRenderCounters& Counters)
		-> FSceneViewStatistics
	{
		FSceneViewStatistics Result;
		Result.Visibility.SubmittedPrimitives = Counters.Visibility.SubmittedPrimitives;
		Result.Visibility.VisiblePrimitives = Counters.Visibility.VisiblePrimitives;
		Result.StaticMesh.Primitives = Counters.StaticMesh.PreparedStaticMeshPrimitives;
		Result.SplineMesh.Primitives = Counters.SplineMesh.PreparedSplineMeshPrimitives;
		Result.SkeletalMesh.Primitives = Counters.SkeletalMesh.PreparedSkeletalMeshPrimitives;
		Result.Terrain.VisiblePatches = Counters.Terrain.VisibleTerrainPatches;

		Result.SplineMesh.Triangles = Counters.SplineMesh.PreparedSplineMeshTriangles;
		Result.StaticMesh.Triangles = Counters.StaticMesh.PreparedStaticMeshTriangles
									  - std::min(Counters.StaticMesh.PreparedStaticMeshTriangles, Counters.SplineMesh.PreparedSplineMeshTriangles);
		Result.SkeletalMesh.Triangles = Counters.SkeletalMesh.PreparedSkeletalMeshTriangles;
		Result.Terrain.Triangles = Counters.Terrain.PreparedTerrainTriangles;
		Result.Summary.Triangles = AddSaturated(
			AddSaturated(Result.StaticMesh.Triangles, Result.SplineMesh.Triangles),
			AddSaturated(Result.SkeletalMesh.Triangles, Result.Terrain.Triangles)
		);
		Result.Shadow.Triangles = Counters.DirectionalShadow.ShadowPreparedTriangles;

		Result.StaticMesh.DrawCalls = Counters.StaticMesh.StaticMeshSuccessfulDraws;
		Result.SkeletalMesh.DrawCalls = Counters.SkeletalMesh.SkeletalMeshSuccessfulDraws;
		Result.Terrain.DrawCalls = Counters.Terrain.TerrainSuccessfulDraws;
		Result.Shadow.DrawCalls = Counters.DirectionalShadow.ShadowSuccessfulDraws;
		Result.Lights.Directional = Counters.Lighting.SelectedDirectionalLights;
		Result.Lights.Point = Counters.Lighting.SelectedPointLights;
		Result.Lights.Spot = Counters.Lighting.SelectedSpotLights;
		Result.Shadow.Cascades = static_cast<uint32>(std::min<size_t>(
			Counters.DirectionalShadow.ShadowCascadeCount, std::numeric_limits<uint32>::max()
		));
		Result.Shadow.bEnabled = Counters.DirectionalShadow.ShadowValidReceiverViews != 0
								 && Counters.DirectionalShadow.ShadowCascadeCount != 0;
		Result.Shadow.bContactEnabled = Counters.ContactShadow.ContactShadowEnabledViews != 0;
		if (Counters.ContactShadow.ContactShadowComputeViews != 0)
			Result.Shadow.ContactRoute = EContactShadowExecutionRoute::Compute;
		else if (Counters.ContactShadow.ContactShadowFragmentViews != 0)
			Result.Shadow.ContactRoute = EContactShadowExecutionRoute::Fragment;
		auto& Cloud = Result.VolumetricCloud;
		Cloud.Quality = Counters.VolumetricCloud.VolumetricCloudQuality;
		Cloud.DebugMode = Counters.VolumetricCloud.VolumetricCloudDebugMode;
		Cloud.TargetWidth = Counters.VolumetricCloud.VolumetricCloudTargetWidth;
		Cloud.TargetHeight = Counters.VolumetricCloud.VolumetricCloudTargetHeight;
		Cloud.OutputWidth = Counters.VolumetricCloud.VolumetricCloudOutputWidth;
		Cloud.OutputHeight = Counters.VolumetricCloud.VolumetricCloudOutputHeight;
		Cloud.PrimarySamples = Counters.VolumetricCloud.VolumetricCloudPrimarySamples;
		Cloud.LightSamples = Counters.VolumetricCloud.VolumetricCloudLightSamples;
		Cloud.ShadowSamples = Counters.VolumetricCloud.VolumetricCloudShadowSamples;
		Cloud.ActiveBytes = Counters.VolumetricCloud.VolumetricCloudActiveBytes;
		Cloud.RetainedBytes = Counters.VolumetricCloud.VolumetricCloudRetainedBytes;
		Cloud.HistoryBytes = Counters.VolumetricCloud.VolumetricCloudHistoryBytes;
		Cloud.ShadowActiveBytes = Counters.VolumetricCloud.VolumetricCloudShadowActiveBytes;
		Cloud.ShadowRetainedBytes = Counters.VolumetricCloud.VolumetricCloudShadowRetainedBytes;
		Cloud.bEnabled = Counters.VolumetricCloud.VolumetricCloudEnabledViews != 0;
		Cloud.bHistoryAvailable = Counters.VolumetricCloud.VolumetricCloudTemporalDraws != 0;
		Cloud.bHistoryAccepted = Counters.VolumetricCloud.VolumetricCloudHistoryAccepted != 0;
		if (Counters.VolumetricCloud.VolumetricCloudComputeViews != 0)
			Cloud.Route = EVolumetricCloudExecutionRoute::Compute;
		else if (Counters.VolumetricCloud.VolumetricCloudFragmentViews != 0)
			Cloud.Route = EVolumetricCloudExecutionRoute::Fragment;
		if (Counters.VolumetricCloud.VolumetricCloudShadowComputeViews != 0)
			Cloud.ShadowRoute = EVolumetricCloudExecutionRoute::Compute;
		else if (Counters.VolumetricCloud.VolumetricCloudShadowFragmentViews != 0)
			Cloud.ShadowRoute = EVolumetricCloudExecutionRoute::Fragment;
		for (size_t Index = 0; Index < Counters.VolumetricCloud.VolumetricCloudRouteReasons.size(); ++Index)
		{
			if (Counters.VolumetricCloud.VolumetricCloudRouteReasons[Index] == 0) continue;
			Cloud.Reason = Index <= static_cast<size_t>(
				EVolumetricCloudRouteReason::FragmentTargetUnavailable)
				? static_cast<EVolumetricCloudRouteReason>(Index)
				: EVolumetricCloudRouteReason::Unknown;
			break;
		}
		return Result;
	}

	auto FSceneTelemetryPublication::Commit() -> void
	{
		if (bCommitted) return;
		EmitViewRenderCounterSnapshot(Telemetry.Counters);
		if (OutStatistics != nullptr)
			*OutStatistics = BuildSceneViewStatistics(Telemetry.Counters);
		bCommitted = true;
	}

	auto ReduceStaticMeshTelemetry(
		const FPreparedStaticMeshView& StaticMeshes,
		const FResolvedStaticMeshView& Resolved,
		FViewRenderCounters& Counters
	) -> void
	{
		Counters.StaticMesh.VisibleStaticMeshCandidates = StaticMeshes.VisibleLocalCandidates;
		Counters.StaticMesh.PreparedStaticMeshPrimitives = StaticMeshes.PreparedLocalPrimitives;
		Counters.StaticMesh.RejectedStaticMeshPrimitives = StaticMeshes.RejectedPrimitives
												- std::min(StaticMeshes.RejectedPrimitives, StaticMeshes.RejectedSplinePrimitives);
		Counters.SplineMesh.VisibleSplineMeshCandidates = StaticMeshes.VisibleSplineCandidates;
		Counters.SplineMesh.PreparedSplineMeshPrimitives = StaticMeshes.PreparedSplinePrimitives;
		Counters.SplineMesh.RejectedSplineMeshPrimitives = StaticMeshes.RejectedSplinePrimitives;
		Counters.SplineMesh.PreparedSplineMeshSections = StaticMeshes.PreparedSplineSections;
		Counters.SplineMesh.PreparedSplineMeshTriangles = StaticMeshes.PreparedSplineTriangles;
		Counters.SplineMesh.RetainedSplineMeshDeformationBytes = StaticMeshes.RetainedSplineDeformationBytes;
		Counters.SplineMesh.AcceptedSplineMeshDynamicUpdates = StaticMeshes.AcceptedSplineDynamicUpdates;
		Counters.StaticMesh.PreparedStaticMeshSections = StaticMeshes.SelectedSections;
		Counters.StaticMesh.PreparedStaticMeshTriangles = StaticMeshes.SelectedTriangles;
		Counters.StaticMesh.StaticMeshProjectedSizeFallbacks =
			StaticMeshes.ProjectedSizeFallbacks;
		Counters.StaticMesh.StaticMeshResourceFallbacks = StaticMeshes.ResourceFallbacks;
		Counters.StaticMesh.RequestedStaticMeshLODHistogram =
			StaticMeshes.RequestedLODHistogram;
		Counters.StaticMesh.SelectedStaticMeshLODHistogram =
			StaticMeshes.SelectedLODHistogram;
		Counters.StaticMesh.OpaqueStaticMeshSections = StaticMeshes.OpaqueSections;
		Counters.StaticMesh.MaskedStaticMeshSections = StaticMeshes.MaskedSections;
		Counters.StaticMesh.TranslucentStaticMeshSections =
			StaticMeshes.TranslucentSections;
		Counters.StaticMesh.OpaqueStaticMeshTriangles = StaticMeshes.OpaqueTriangles;
		Counters.StaticMesh.MaskedStaticMeshTriangles = StaticMeshes.MaskedTriangles;
		Counters.StaticMesh.TranslucentStaticMeshTriangles =
			StaticMeshes.TranslucentTriangles;
		Counters.StaticMesh.OpaqueStaticMeshStateGroups = StaticMeshes.OpaqueStateGroups;
		Counters.StaticMesh.MaskedStaticMeshStateGroups = StaticMeshes.MaskedStateGroups;
		Counters.StaticMesh.OpaqueStaticMeshInputStateGroups =
			StaticMeshes.OpaqueInputStateGroups;
		Counters.StaticMesh.MaskedStaticMeshInputStateGroups =
			StaticMeshes.MaskedInputStateGroups;
		Counters.StaticMesh.StaticMeshPipelineTransitions =
			StaticMeshes.PipelineTransitions;
		Counters.StaticMesh.StaticMeshMaterialTransitions =
			StaticMeshes.MaterialTransitions;
		Counters.StaticMesh.StaticMeshVertexFactoryTransitions =
			StaticMeshes.VertexFactoryTransitions;
		Counters.StaticMesh.StaticMeshGeometryTransitions =
			StaticMeshes.GeometryTransitions;
		Counters.StaticMesh.StaticMeshResourceAttemptedDraws =
			Resolved.Observations.ResourcePreparationAttemptedDraws;
		Counters.StaticMesh.StaticMeshResourceSuccessfulDraws =
			Resolved.Observations.ResourcePreparationSuccessfulDraws;
		Counters.StaticMesh.StaticMeshResourceRejectedDraws =
			Resolved.Observations.ResourcePreparationRejectedDraws;
		Counters.StaticMesh.StaticMeshAttemptedDraws = Resolved.Observations.AttemptedDraws;
		Counters.StaticMesh.StaticMeshSuccessfulDraws = Resolved.Observations.SuccessfulDraws;
		Counters.StaticMesh.StaticMeshRejectedDraws = Resolved.Observations.RejectedDraws;
	}

	auto ReduceSkeletalMeshTelemetry(
		const FPreparedSkeletalMeshView& Meshes,
		const FResolvedSkeletalMeshView& Resolved,
		const FResolvedSkeletalPaletteTable& Palettes,
		FViewRenderCounters& Counters
	) -> void
	{
		check(Palettes.RequestedPalettes == Palettes.UploadedPalettes + Palettes.ReusedPalettes + Palettes.RejectedPalettes);
		check(Palettes.UploadedBytes == Palettes.UploadedMatrices * sizeof(FMatrix4f));
		Counters.SkeletalMesh.PreparedSkeletalMeshPrimitives = Meshes.Primitives.size();
		Counters.SkeletalMesh.RejectedSkeletalMeshPrimitives = Meshes.RejectedPrimitives;
		Counters.SkeletalMesh.PreparedSkeletalMeshSections = Meshes.SelectedSections;
		Counters.SkeletalMesh.PreparedSkeletalMeshTriangles = Meshes.SelectedTriangles;
		Counters.SkeletalMesh.OpaqueSkeletalMeshSections = Meshes.OpaqueSections;
		Counters.SkeletalMesh.MaskedSkeletalMeshSections = Meshes.MaskedSections;
		Counters.SkeletalMesh.TranslucentSkeletalMeshSections = Meshes.TranslucentSections;
		Counters.SkeletalMesh.OpaqueSkeletalMeshTriangles = Meshes.OpaqueTriangles;
		Counters.SkeletalMesh.MaskedSkeletalMeshTriangles = Meshes.MaskedTriangles;
		Counters.SkeletalMesh.TranslucentSkeletalMeshTriangles = Meshes.TranslucentTriangles;
		Counters.SkeletalMesh.OpaqueSkeletalMeshStateGroups = Meshes.OpaqueStateGroups;
		Counters.SkeletalMesh.MaskedSkeletalMeshStateGroups = Meshes.MaskedStateGroups;
		Counters.SkeletalMesh.SkeletalMeshPipelineTransitions = Meshes.PipelineTransitions;
		Counters.SkeletalMesh.SkeletalMeshMaterialTransitions = Meshes.MaterialTransitions;
		Counters.SkeletalMesh.SkeletalMeshVertexFactoryTransitions =
			Meshes.VertexFactoryTransitions;
		Counters.SkeletalMesh.SkeletalMeshGeometryTransitions = Meshes.GeometryTransitions;
		Counters.SkeletalMesh.SkeletalMeshResourceAttemptedDraws =
			Resolved.Observations.ResourcePreparationAttemptedDraws;
		Counters.SkeletalMesh.SkeletalMeshResourceSuccessfulDraws =
			Resolved.Observations.ResourcePreparationSuccessfulDraws;
		Counters.SkeletalMesh.SkeletalMeshResourceRejectedDraws =
			Resolved.Observations.ResourcePreparationRejectedDraws;
		Counters.SkeletalMesh.SkeletalMeshAttemptedDraws = Resolved.Observations.AttemptedDraws;
		Counters.SkeletalMesh.SkeletalMeshSuccessfulDraws = Resolved.Observations.SuccessfulDraws;
		Counters.SkeletalMesh.SkeletalMeshRejectedDraws = Resolved.Observations.RejectedDraws;
		Counters.SkeletalMesh.RequestedSkeletalPaletteUploads = Palettes.RequestedPalettes;
		Counters.SkeletalMesh.UploadedSkeletalPalettes = Palettes.UploadedPalettes;
		Counters.SkeletalMesh.ReusedSkeletalPalettes = Palettes.ReusedPalettes;
		Counters.SkeletalMesh.RejectedSkeletalPalettes = Palettes.RejectedPalettes;
		Counters.SkeletalMesh.UploadedSkeletalPaletteMatrices = Palettes.UploadedMatrices;
		Counters.SkeletalMesh.UploadedSkeletalPaletteBytes = Palettes.UploadedBytes;
	}

	auto ReduceTerrainTelemetry(
		const FPreparedTerrainView& Terrain,
		const FResolvedTerrainView& Resolved,
		FViewRenderCounters& Counters
	) -> void
	{
		Counters.Terrain.TerrainPatchCandidates = Terrain.PatchCandidates;
		Counters.Terrain.VisibleTerrainPatches = Terrain.VisiblePatches;
		Counters.Terrain.CulledTerrainPatches = Terrain.CulledPatches;
		Counters.Terrain.InnerTerrainPatches = Terrain.InnerPatches;
		Counters.Terrain.TransitionTerrainPatches = Terrain.TransitionPatches;
		Counters.Terrain.RadialRejectedTerrainPatches = Terrain.RadialRejectedPatches;
		Counters.Terrain.InvalidTerrainDistanceSettingFallbacks =
			Terrain.InvalidDistanceSettingFallbacks;
		Counters.Terrain.InvalidTerrainPatchBounds = Terrain.InvalidBoundsFallbacks;
		Counters.Terrain.TerrainLODFallbacks = Terrain.LODFallbacks;
		Counters.Terrain.TerrainLODResolutionFallbacks = Terrain.LODResolutionFallbacks;
		Counters.Terrain.TerrainAdjacencyPromotions = Terrain.AdjacencyPromotions;
		Counters.Terrain.TerrainAdjacencyIterations = Terrain.AdjacencyIterations;
		Counters.Terrain.RequestedTerrainLODHistogram = Terrain.RequestedLODHistogram;
		Counters.Terrain.ResolvedTerrainLODHistogram = Terrain.ResolvedLODHistogram;
		Counters.Terrain.TerrainStitchMaskHistogram = Terrain.StitchMaskHistogram;
		Counters.Terrain.PreparedTerrainTriangles = Terrain.Triangles;
		Counters.Terrain.OpaqueTerrainPatches = Terrain.Opaque.size();
		Counters.Terrain.MaskedTerrainPatches = Terrain.Masked.size();
		Counters.Terrain.TranslucentTerrainPatches = Terrain.Translucent.size();
		Counters.Terrain.TerrainResourceAttemptedDraws = Resolved.Observations.ResourceAttemptedDraws;
		Counters.Terrain.TerrainResourceSuccessfulDraws = Resolved.Observations.ResourceSuccessfulDraws;
		Counters.Terrain.TerrainResourceRejectedDraws = Resolved.Observations.ResourceRejectedDraws;
		Counters.Terrain.PreparedTerrainBatches = Resolved.Observations.PreparedBatches;
		Counters.Terrain.TerrainBatchChunks = Resolved.Observations.BatchChunks;
		Counters.Terrain.TerrainInstances = Resolved.Observations.InstanceCount;
		Counters.Terrain.TerrainInstanceBytes = Resolved.Observations.InstanceBytes;
		Counters.Terrain.TerrainInstanceAllocations = Resolved.Observations.InstanceAllocations;
		Counters.Terrain.TerrainResourceAttemptedBatches = Resolved.Observations.ResourceAttemptedBatches;
		Counters.Terrain.TerrainResourceSuccessfulBatches = Resolved.Observations.ResourceSuccessfulBatches;
		Counters.Terrain.TerrainResourceRejectedBatches = Resolved.Observations.ResourceRejectedBatches;
		Counters.Terrain.TerrainSubmittedLogicalPatches = Resolved.Observations.SubmittedLogicalPatches;
		Counters.Terrain.TerrainScalarTranslucentDraws = Resolved.Observations.ScalarTranslucentDraws;
		Counters.Terrain.TerrainLogicalPreparationNanoseconds = Terrain.LogicalPreparationNanoseconds;
		Counters.Terrain.TerrainBatchConstructionNanoseconds = Terrain.BatchConstructionNanoseconds;
		Counters.Terrain.TerrainResourcePreparationNanoseconds = Resolved.Observations.ResourcePreparationNanoseconds;
		Counters.Terrain.TerrainHeightPreparationNanoseconds = Resolved.Observations.HeightPreparationNanoseconds;
		Counters.Terrain.TerrainTopologyPreparationNanoseconds = Resolved.Observations.TopologyPreparationNanoseconds;
		Counters.Terrain.TerrainShaderPreparationNanoseconds = Resolved.Observations.ShaderPreparationNanoseconds;
		Counters.Terrain.TerrainPipelinePreparationNanoseconds = Resolved.Observations.PipelinePreparationNanoseconds;
		Counters.Terrain.TerrainDynamicAllocationNanoseconds = Resolved.Observations.DynamicAllocationNanoseconds;
		Counters.Terrain.TerrainCommandRecordingNanoseconds = Resolved.Observations.CommandRecordingNanoseconds;
		Counters.Terrain.TerrainAttemptedDraws = Resolved.Observations.AttemptedDraws;
		Counters.Terrain.TerrainSuccessfulDraws = Resolved.Observations.SuccessfulDraws;
		Counters.Terrain.TerrainRejectedDraws = Resolved.Observations.RejectedDraws;
		Counters.Terrain.TerrainHeightUploadBytes = Resolved.Observations.HeightUploadBytes;
		Counters.Terrain.TerrainHeightUploads = Resolved.Observations.HeightUploads;
		Counters.Terrain.TerrainHeightReuses = Resolved.Observations.HeightReuses;
		Counters.Terrain.TerrainTopologyCreations = Resolved.Observations.TopologyCreations;
		Counters.Terrain.TerrainTopologyReuses = Resolved.Observations.TopologyReuses;
		Counters.Terrain.TerrainTopologyBytes = Resolved.Observations.TopologyBytes;
		Counters.Terrain.TerrainShaderLookups = Resolved.Observations.ShaderLookups;
		Counters.Terrain.TerrainShaderCreations = Resolved.Observations.ShaderCreations;
		Counters.Terrain.TerrainShaderReuses = Resolved.Observations.ShaderReuses;
		Counters.Terrain.TerrainPipelineLookups = Resolved.Observations.PipelineLookups;
		Counters.Terrain.TerrainPipelineCreations = Resolved.Observations.PipelineCreations;
		Counters.Terrain.TerrainPipelineReuses = Resolved.Observations.PipelineReuses;
	}
} // namespace Durin
