#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SkeletalMeshRenderPreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/TerrainRenderPreparation.h"

namespace Durin
{
	namespace
	{
		auto AddSaturated(uint64 A, uint64 B) -> uint64
		{
			return B > std::numeric_limits<uint64>::max() - A
				? std::numeric_limits<uint64>::max() : A + B;
		}
	}

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
		auto& Cloud = Result.VolumetricCloud;
		Cloud.Quality = Counters.VolumetricCloudQuality;
		Cloud.DebugMode = Counters.VolumetricCloudDebugMode;
		Cloud.TargetWidth = Counters.VolumetricCloudTargetWidth;
		Cloud.TargetHeight = Counters.VolumetricCloudTargetHeight;
		Cloud.OutputWidth = Counters.VolumetricCloudOutputWidth;
		Cloud.OutputHeight = Counters.VolumetricCloudOutputHeight;
		Cloud.PrimarySamples = Counters.VolumetricCloudPrimarySamples;
		Cloud.LightSamples = Counters.VolumetricCloudLightSamples;
		Cloud.ShadowSamples = Counters.VolumetricCloudShadowSamples;
		Cloud.ActiveBytes = Counters.VolumetricCloudActiveBytes;
		Cloud.RetainedBytes = Counters.VolumetricCloudRetainedBytes;
		Cloud.HistoryBytes = Counters.VolumetricCloudHistoryBytes;
		Cloud.ShadowActiveBytes = Counters.VolumetricCloudShadowActiveBytes;
		Cloud.ShadowRetainedBytes = Counters.VolumetricCloudShadowRetainedBytes;
		Cloud.bEnabled = Counters.VolumetricCloudEnabledViews != 0;
		Cloud.bHistoryAvailable = Counters.VolumetricCloudTemporalDraws != 0;
		Cloud.bHistoryAccepted = Counters.VolumetricCloudHistoryAccepted != 0;
		if (Counters.VolumetricCloudComputeViews != 0)
			Cloud.Route = EVolumetricCloudExecutionRoute::Compute;
		else if (Counters.VolumetricCloudFragmentViews != 0)
			Cloud.Route = EVolumetricCloudExecutionRoute::Fragment;
		if (Counters.VolumetricCloudShadowComputeViews != 0)
			Cloud.ShadowRoute = EVolumetricCloudExecutionRoute::Compute;
		else if (Counters.VolumetricCloudShadowFragmentViews != 0)
			Cloud.ShadowRoute = EVolumetricCloudExecutionRoute::Fragment;
		for (size_t Index = 0; Index < Counters.VolumetricCloudRouteReasons.size(); ++Index)
		{
			if (Counters.VolumetricCloudRouteReasons[Index] == 0) continue;
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
			Resolved.ResourcePreparationAttemptedDraws;
		Counters.StaticMeshResourceSuccessfulDraws =
			Resolved.ResourcePreparationSuccessfulDraws;
		Counters.StaticMeshResourceRejectedDraws =
			Resolved.ResourcePreparationRejectedDraws;
		Counters.StaticMeshAttemptedDraws = Resolved.AttemptedDraws;
		Counters.StaticMeshSuccessfulDraws = Resolved.SuccessfulDraws;
		Counters.StaticMeshRejectedDraws = Resolved.RejectedDraws;
	}

	auto ReduceSkeletalMeshTelemetry(
		const FPreparedSkeletalMeshView& Meshes,
		const FResolvedSkeletalMeshView& Resolved,
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
			Resolved.ResourcePreparationAttemptedDraws;
		Counters.SkeletalMeshResourceSuccessfulDraws =
			Resolved.ResourcePreparationSuccessfulDraws;
		Counters.SkeletalMeshResourceRejectedDraws =
			Resolved.ResourcePreparationRejectedDraws;
		Counters.SkeletalMeshAttemptedDraws = Resolved.AttemptedDraws;
		Counters.SkeletalMeshSuccessfulDraws = Resolved.SuccessfulDraws;
		Counters.SkeletalMeshRejectedDraws = Resolved.RejectedDraws;
		Counters.RequestedSkeletalPaletteUploads = Palettes.RequestedPalettes;
		Counters.UploadedSkeletalPalettes = Palettes.UploadedPalettes;
		Counters.ReusedSkeletalPalettes = Palettes.ReusedPalettes;
		Counters.RejectedSkeletalPalettes = Palettes.RejectedPalettes;
		Counters.UploadedSkeletalPaletteMatrices = Palettes.UploadedMatrices;
		Counters.UploadedSkeletalPaletteBytes = Palettes.UploadedBytes;
	}

	auto ReduceTerrainTelemetry(
		const FPreparedTerrainView& Terrain,
		const FResolvedTerrainView& Resolved,
		FViewRenderCounters& Counters
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
		Counters.TerrainResourceAttemptedDraws = Resolved.ResourceAttemptedDraws;
		Counters.TerrainResourceSuccessfulDraws = Resolved.ResourceSuccessfulDraws;
		Counters.TerrainResourceRejectedDraws = Resolved.ResourceRejectedDraws;
		Counters.PreparedTerrainBatches = Resolved.PreparedBatches;
		Counters.TerrainBatchChunks = Resolved.BatchChunks;
		Counters.TerrainInstances = Resolved.InstanceCount;
		Counters.TerrainInstanceBytes = Resolved.InstanceBytes;
		Counters.TerrainInstanceAllocations = Resolved.InstanceAllocations;
		Counters.TerrainResourceAttemptedBatches = Resolved.ResourceAttemptedBatches;
		Counters.TerrainResourceSuccessfulBatches = Resolved.ResourceSuccessfulBatches;
		Counters.TerrainResourceRejectedBatches = Resolved.ResourceRejectedBatches;
		Counters.TerrainSubmittedLogicalPatches = Resolved.SubmittedLogicalPatches;
		Counters.TerrainScalarTranslucentDraws = Resolved.ScalarTranslucentDraws;
		Counters.TerrainLogicalPreparationNanoseconds = Terrain.LogicalPreparationNanoseconds;
		Counters.TerrainBatchConstructionNanoseconds = Terrain.BatchConstructionNanoseconds;
		Counters.TerrainResourcePreparationNanoseconds = Resolved.ResourcePreparationNanoseconds;
		Counters.TerrainHeightPreparationNanoseconds = Resolved.HeightPreparationNanoseconds;
		Counters.TerrainTopologyPreparationNanoseconds = Resolved.TopologyPreparationNanoseconds;
		Counters.TerrainShaderPreparationNanoseconds = Resolved.ShaderPreparationNanoseconds;
		Counters.TerrainPipelinePreparationNanoseconds = Resolved.PipelinePreparationNanoseconds;
		Counters.TerrainDynamicAllocationNanoseconds = Resolved.DynamicAllocationNanoseconds;
		Counters.TerrainCommandRecordingNanoseconds = Resolved.CommandRecordingNanoseconds;
		Counters.TerrainAttemptedDraws = Resolved.AttemptedDraws;
		Counters.TerrainSuccessfulDraws = Resolved.SuccessfulDraws;
		Counters.TerrainRejectedDraws = Resolved.RejectedDraws;
		Counters.TerrainHeightUploadBytes = Resolved.HeightUploadBytes;
		Counters.TerrainHeightUploads = Resolved.HeightUploads;
		Counters.TerrainHeightReuses = Resolved.HeightReuses;
		Counters.TerrainTopologyCreations = Resolved.TopologyCreations;
		Counters.TerrainTopologyReuses = Resolved.TopologyReuses;
		Counters.TerrainTopologyBytes = Resolved.TopologyBytes;
		Counters.TerrainShaderLookups = Resolved.ShaderLookups;
		Counters.TerrainShaderCreations = Resolved.ShaderCreations;
		Counters.TerrainShaderReuses = Resolved.ShaderReuses;
		Counters.TerrainPipelineLookups = Resolved.PipelineLookups;
		Counters.TerrainPipelineCreations = Resolved.PipelineCreations;
		Counters.TerrainPipelineReuses = Resolved.PipelineReuses;
	}
} // namespace Durin
