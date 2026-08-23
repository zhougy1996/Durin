#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/SkeletalMeshRenderPreparation.h"
#include "Renderers/StaticMeshRenderPreparation.h"
#include "Renderers/TerrainRenderPreparation.h"

namespace Durin
{
	namespace
	{
		std::atomic<FViewRenderTelemetrySink> GViewRenderTelemetrySink = nullptr;

		auto AddSaturated(uint64 A, uint64 B) -> uint64
		{
			return B > std::numeric_limits<uint64>::max() - A
				? std::numeric_limits<uint64>::max() : A + B;
		}
	}

	auto SetViewRenderTelemetrySink(FViewRenderTelemetrySink Sink) -> void
	{
		GViewRenderTelemetrySink.store(Sink, std::memory_order_release);
	}

	auto EmitViewRenderTelemetrySnapshot(
		const FViewRenderTelemetry& Telemetry) -> void
	{
		if (const FViewRenderTelemetrySink Sink =
			GViewRenderTelemetrySink.load(std::memory_order_acquire))
		{
			Sink(Telemetry);
		}
	}

	auto BuildSceneViewStatistics(const FViewRenderTelemetry& Telemetry)
		-> FSceneViewStatistics
	{
		FSceneViewStatistics Result;
		Result.Visibility.SubmittedPrimitives = Telemetry.Visibility.SubmittedPrimitives;
		Result.Visibility.VisiblePrimitives = Telemetry.Visibility.VisiblePrimitives;
		Result.StaticMesh.Primitives = Telemetry.StaticMesh.PreparedStaticMeshPrimitives;
		Result.SplineMesh.Primitives = Telemetry.SplineMesh.PreparedSplineMeshPrimitives;
		Result.SkeletalMesh.Primitives = Telemetry.SkeletalMesh.PreparedSkeletalMeshPrimitives;
		Result.Terrain.VisiblePatches = Telemetry.Terrain.VisibleTerrainPatches;

		Result.SplineMesh.Triangles = Telemetry.SplineMesh.PreparedSplineMeshTriangles;
		Result.StaticMesh.Triangles = Telemetry.StaticMesh.PreparedStaticMeshTriangles
									  - std::min(Telemetry.StaticMesh.PreparedStaticMeshTriangles, Telemetry.SplineMesh.PreparedSplineMeshTriangles);
		Result.SkeletalMesh.Triangles = Telemetry.SkeletalMesh.PreparedSkeletalMeshTriangles;
		Result.Terrain.Triangles = Telemetry.Terrain.PreparedTerrainTriangles;
		Result.Summary.Triangles = AddSaturated(
			AddSaturated(Result.StaticMesh.Triangles, Result.SplineMesh.Triangles),
			AddSaturated(Result.SkeletalMesh.Triangles, Result.Terrain.Triangles)
		);
		Result.Shadow.Triangles = Telemetry.DirectionalShadow.ShadowPreparedTriangles;

		Result.StaticMesh.DrawCalls = Telemetry.StaticMesh.StaticMeshSuccessfulDraws;
		Result.SkeletalMesh.DrawCalls = Telemetry.SkeletalMesh.SkeletalMeshSuccessfulDraws;
		Result.Terrain.DrawCalls = Telemetry.Terrain.TerrainSuccessfulDraws;
		Result.Shadow.DrawCalls = Telemetry.DirectionalShadow.ShadowSuccessfulDraws;
		Result.Lights.Directional = Telemetry.Lighting.SelectedDirectionalLights;
		Result.Lights.Point = Telemetry.Lighting.SelectedPointLights;
		Result.Lights.Spot = Telemetry.Lighting.SelectedSpotLights;
		Result.Shadow.Cascades = static_cast<uint32>(std::min<size_t>(
			Telemetry.DirectionalShadow.ShadowCascadeCount, std::numeric_limits<uint32>::max()
		));
		Result.Shadow.bEnabled = Telemetry.DirectionalShadow.ShadowValidReceiverViews != 0
								 && Telemetry.DirectionalShadow.ShadowCascadeCount != 0;
		Result.Shadow.bContactEnabled = Telemetry.ContactShadow.ContactShadowEnabledViews != 0;
		if (Telemetry.ContactShadow.ContactShadowComputeViews != 0)
			Result.Shadow.ContactRoute = EContactShadowExecutionRoute::Compute;
		else if (Telemetry.ContactShadow.ContactShadowFragmentViews != 0)
			Result.Shadow.ContactRoute = EContactShadowExecutionRoute::Fragment;
		auto& Cloud = Result.VolumetricCloud;
		Cloud.Quality = Telemetry.VolumetricCloud.VolumetricCloudQuality;
		Cloud.DebugMode = Telemetry.VolumetricCloud.VolumetricCloudDebugMode;
		Cloud.TargetWidth = Telemetry.VolumetricCloud.VolumetricCloudTargetWidth;
		Cloud.TargetHeight = Telemetry.VolumetricCloud.VolumetricCloudTargetHeight;
		Cloud.OutputWidth = Telemetry.VolumetricCloud.VolumetricCloudOutputWidth;
		Cloud.OutputHeight = Telemetry.VolumetricCloud.VolumetricCloudOutputHeight;
		Cloud.PrimarySamples = Telemetry.VolumetricCloud.VolumetricCloudPrimarySamples;
		Cloud.LightSamples = Telemetry.VolumetricCloud.VolumetricCloudLightSamples;
		Cloud.ShadowSamples = Telemetry.VolumetricCloud.VolumetricCloudShadowSamples;
		Cloud.ActiveBytes = Telemetry.VolumetricCloud.VolumetricCloudActiveBytes;
		Cloud.RetainedBytes = Telemetry.VolumetricCloud.VolumetricCloudRetainedBytes;
		Cloud.HistoryBytes = Telemetry.VolumetricCloud.VolumetricCloudHistoryBytes;
		Cloud.ShadowActiveBytes = Telemetry.VolumetricCloud.VolumetricCloudShadowActiveBytes;
		Cloud.ShadowRetainedBytes = Telemetry.VolumetricCloud.VolumetricCloudShadowRetainedBytes;
		Cloud.bEnabled = Telemetry.VolumetricCloud.VolumetricCloudEnabledViews != 0;
		Cloud.bHistoryAvailable = Telemetry.VolumetricCloud.VolumetricCloudTemporalDraws != 0;
		Cloud.bHistoryAccepted = Telemetry.VolumetricCloud.VolumetricCloudHistoryAccepted != 0;
		if (Telemetry.VolumetricCloud.VolumetricCloudComputeViews != 0)
			Cloud.Route = EVolumetricCloudExecutionRoute::Compute;
		else if (Telemetry.VolumetricCloud.VolumetricCloudFragmentViews != 0)
			Cloud.Route = EVolumetricCloudExecutionRoute::Fragment;
		if (Telemetry.VolumetricCloud.VolumetricCloudShadowComputeViews != 0)
			Cloud.ShadowRoute = EVolumetricCloudExecutionRoute::Compute;
		else if (Telemetry.VolumetricCloud.VolumetricCloudShadowFragmentViews != 0)
			Cloud.ShadowRoute = EVolumetricCloudExecutionRoute::Fragment;
		for (size_t Index = 0; Index < Telemetry.VolumetricCloud.VolumetricCloudRouteReasons.size(); ++Index)
		{
			if (Telemetry.VolumetricCloud.VolumetricCloudRouteReasons[Index] == 0) continue;
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
		EmitViewRenderTelemetrySnapshot(Telemetry.View);
		if (OutStatistics != nullptr)
			*OutStatistics = BuildSceneViewStatistics(Telemetry.View);
		bCommitted = true;
	}

	auto ReduceStaticMeshTelemetry(
		const FPreparedStaticMeshView& StaticMeshes,
		const FResolvedStaticMeshView& Resolved,
		FViewRenderTelemetry& Telemetry
	) -> void
	{
		Telemetry.StaticMesh.VisibleStaticMeshCandidates = StaticMeshes.VisibleLocalCandidates;
		Telemetry.StaticMesh.PreparedStaticMeshPrimitives = StaticMeshes.PreparedLocalPrimitives;
		Telemetry.StaticMesh.RejectedStaticMeshPrimitives = StaticMeshes.RejectedPrimitives
												- std::min(StaticMeshes.RejectedPrimitives, StaticMeshes.RejectedSplinePrimitives);
		Telemetry.SplineMesh.VisibleSplineMeshCandidates = StaticMeshes.VisibleSplineCandidates;
		Telemetry.SplineMesh.PreparedSplineMeshPrimitives = StaticMeshes.PreparedSplinePrimitives;
		Telemetry.SplineMesh.RejectedSplineMeshPrimitives = StaticMeshes.RejectedSplinePrimitives;
		Telemetry.SplineMesh.PreparedSplineMeshSections = StaticMeshes.PreparedSplineSections;
		Telemetry.SplineMesh.PreparedSplineMeshTriangles = StaticMeshes.PreparedSplineTriangles;
		Telemetry.SplineMesh.RetainedSplineMeshDeformationBytes = StaticMeshes.RetainedSplineDeformationBytes;
		Telemetry.SplineMesh.AcceptedSplineMeshDynamicUpdates = StaticMeshes.AcceptedSplineDynamicUpdates;
		Telemetry.StaticMesh.PreparedStaticMeshSections = StaticMeshes.SelectedSections;
		Telemetry.StaticMesh.PreparedStaticMeshTriangles = StaticMeshes.SelectedTriangles;
		Telemetry.StaticMesh.StaticMeshProjectedSizeFallbacks =
			StaticMeshes.ProjectedSizeFallbacks;
		Telemetry.StaticMesh.StaticMeshResourceFallbacks = StaticMeshes.ResourceFallbacks;
		Telemetry.StaticMesh.RequestedStaticMeshLODHistogram =
			StaticMeshes.RequestedLODHistogram;
		Telemetry.StaticMesh.SelectedStaticMeshLODHistogram =
			StaticMeshes.SelectedLODHistogram;
		Telemetry.StaticMesh.OpaqueStaticMeshSections = StaticMeshes.OpaqueSections;
		Telemetry.StaticMesh.MaskedStaticMeshSections = StaticMeshes.MaskedSections;
		Telemetry.StaticMesh.TranslucentStaticMeshSections =
			StaticMeshes.TranslucentSections;
		Telemetry.StaticMesh.OpaqueStaticMeshTriangles = StaticMeshes.OpaqueTriangles;
		Telemetry.StaticMesh.MaskedStaticMeshTriangles = StaticMeshes.MaskedTriangles;
		Telemetry.StaticMesh.TranslucentStaticMeshTriangles =
			StaticMeshes.TranslucentTriangles;
		Telemetry.StaticMesh.OpaqueStaticMeshStateGroups = StaticMeshes.OpaqueStateGroups;
		Telemetry.StaticMesh.MaskedStaticMeshStateGroups = StaticMeshes.MaskedStateGroups;
		Telemetry.StaticMesh.OpaqueStaticMeshInputStateGroups =
			StaticMeshes.OpaqueInputStateGroups;
		Telemetry.StaticMesh.MaskedStaticMeshInputStateGroups =
			StaticMeshes.MaskedInputStateGroups;
		Telemetry.StaticMesh.StaticMeshPipelineTransitions =
			StaticMeshes.PipelineTransitions;
		Telemetry.StaticMesh.StaticMeshMaterialTransitions =
			StaticMeshes.MaterialTransitions;
		Telemetry.StaticMesh.StaticMeshVertexFactoryTransitions =
			StaticMeshes.VertexFactoryTransitions;
		Telemetry.StaticMesh.StaticMeshGeometryTransitions =
			StaticMeshes.GeometryTransitions;
		Telemetry.StaticMesh.StaticMeshResourceAttemptedDraws =
			Resolved.Observations.ResourcePreparationAttemptedDraws;
		Telemetry.StaticMesh.StaticMeshResourceSuccessfulDraws =
			Resolved.Observations.ResourcePreparationSuccessfulDraws;
		Telemetry.StaticMesh.StaticMeshResourceRejectedDraws =
			Resolved.Observations.ResourcePreparationRejectedDraws;
		Telemetry.StaticMesh.StaticMeshAttemptedDraws = Resolved.Observations.AttemptedDraws;
		Telemetry.StaticMesh.StaticMeshSuccessfulDraws = Resolved.Observations.SuccessfulDraws;
		Telemetry.StaticMesh.StaticMeshRejectedDraws = Resolved.Observations.RejectedDraws;
	}

	auto ReduceSkeletalMeshTelemetry(
		const FPreparedSkeletalMeshView& Meshes,
		const FResolvedSkeletalMeshView& Resolved,
		const FResolvedSkeletalPaletteTable& Palettes,
		FViewRenderTelemetry& Telemetry
	) -> void
	{
		check(Palettes.RequestedPalettes == Palettes.UploadedPalettes + Palettes.ReusedPalettes + Palettes.RejectedPalettes);
		check(Palettes.UploadedBytes == Palettes.UploadedMatrices * sizeof(FMatrix4f));
		Telemetry.SkeletalMesh.PreparedSkeletalMeshPrimitives = Meshes.Primitives.size();
		Telemetry.SkeletalMesh.RejectedSkeletalMeshPrimitives = Meshes.RejectedPrimitives;
		Telemetry.SkeletalMesh.PreparedSkeletalMeshSections = Meshes.SelectedSections;
		Telemetry.SkeletalMesh.PreparedSkeletalMeshTriangles = Meshes.SelectedTriangles;
		Telemetry.SkeletalMesh.OpaqueSkeletalMeshSections = Meshes.OpaqueSections;
		Telemetry.SkeletalMesh.MaskedSkeletalMeshSections = Meshes.MaskedSections;
		Telemetry.SkeletalMesh.TranslucentSkeletalMeshSections = Meshes.TranslucentSections;
		Telemetry.SkeletalMesh.OpaqueSkeletalMeshTriangles = Meshes.OpaqueTriangles;
		Telemetry.SkeletalMesh.MaskedSkeletalMeshTriangles = Meshes.MaskedTriangles;
		Telemetry.SkeletalMesh.TranslucentSkeletalMeshTriangles = Meshes.TranslucentTriangles;
		Telemetry.SkeletalMesh.OpaqueSkeletalMeshStateGroups = Meshes.OpaqueStateGroups;
		Telemetry.SkeletalMesh.MaskedSkeletalMeshStateGroups = Meshes.MaskedStateGroups;
		Telemetry.SkeletalMesh.SkeletalMeshPipelineTransitions = Meshes.PipelineTransitions;
		Telemetry.SkeletalMesh.SkeletalMeshMaterialTransitions = Meshes.MaterialTransitions;
		Telemetry.SkeletalMesh.SkeletalMeshVertexFactoryTransitions =
			Meshes.VertexFactoryTransitions;
		Telemetry.SkeletalMesh.SkeletalMeshGeometryTransitions = Meshes.GeometryTransitions;
		Telemetry.SkeletalMesh.SkeletalMeshResourceAttemptedDraws =
			Resolved.Observations.ResourcePreparationAttemptedDraws;
		Telemetry.SkeletalMesh.SkeletalMeshResourceSuccessfulDraws =
			Resolved.Observations.ResourcePreparationSuccessfulDraws;
		Telemetry.SkeletalMesh.SkeletalMeshResourceRejectedDraws =
			Resolved.Observations.ResourcePreparationRejectedDraws;
		Telemetry.SkeletalMesh.SkeletalMeshAttemptedDraws = Resolved.Observations.AttemptedDraws;
		Telemetry.SkeletalMesh.SkeletalMeshSuccessfulDraws = Resolved.Observations.SuccessfulDraws;
		Telemetry.SkeletalMesh.SkeletalMeshRejectedDraws = Resolved.Observations.RejectedDraws;
		Telemetry.SkeletalMesh.RequestedSkeletalPaletteUploads = Palettes.RequestedPalettes;
		Telemetry.SkeletalMesh.UploadedSkeletalPalettes = Palettes.UploadedPalettes;
		Telemetry.SkeletalMesh.ReusedSkeletalPalettes = Palettes.ReusedPalettes;
		Telemetry.SkeletalMesh.RejectedSkeletalPalettes = Palettes.RejectedPalettes;
		Telemetry.SkeletalMesh.UploadedSkeletalPaletteMatrices = Palettes.UploadedMatrices;
		Telemetry.SkeletalMesh.UploadedSkeletalPaletteBytes = Palettes.UploadedBytes;
	}

	auto ReduceTerrainTelemetry(
		const FPreparedTerrainView& Terrain,
		const FResolvedTerrainView& Resolved,
		FViewRenderTelemetry& Telemetry
	) -> void
	{
		Telemetry.Terrain.TerrainPatchCandidates = Terrain.PatchCandidates;
		Telemetry.Terrain.VisibleTerrainPatches = Terrain.VisiblePatches;
		Telemetry.Terrain.CulledTerrainPatches = Terrain.CulledPatches;
		Telemetry.Terrain.InnerTerrainPatches = Terrain.InnerPatches;
		Telemetry.Terrain.TransitionTerrainPatches = Terrain.TransitionPatches;
		Telemetry.Terrain.RadialRejectedTerrainPatches = Terrain.RadialRejectedPatches;
		Telemetry.Terrain.InvalidTerrainDistanceSettingFallbacks =
			Terrain.InvalidDistanceSettingFallbacks;
		Telemetry.Terrain.InvalidTerrainPatchBounds = Terrain.InvalidBoundsFallbacks;
		Telemetry.Terrain.TerrainLODFallbacks = Terrain.LODFallbacks;
		Telemetry.Terrain.TerrainLODResolutionFallbacks = Terrain.LODResolutionFallbacks;
		Telemetry.Terrain.TerrainAdjacencyPromotions = Terrain.AdjacencyPromotions;
		Telemetry.Terrain.TerrainAdjacencyIterations = Terrain.AdjacencyIterations;
		Telemetry.Terrain.RequestedTerrainLODHistogram = Terrain.RequestedLODHistogram;
		Telemetry.Terrain.ResolvedTerrainLODHistogram = Terrain.ResolvedLODHistogram;
		Telemetry.Terrain.TerrainStitchMaskHistogram = Terrain.StitchMaskHistogram;
		Telemetry.Terrain.PreparedTerrainTriangles = Terrain.Triangles;
		Telemetry.Terrain.OpaqueTerrainPatches = Terrain.Opaque.size();
		Telemetry.Terrain.MaskedTerrainPatches = Terrain.Masked.size();
		Telemetry.Terrain.TranslucentTerrainPatches = Terrain.Translucent.size();
		Telemetry.Terrain.TerrainResourceAttemptedDraws = Resolved.Observations.ResourceAttemptedDraws;
		Telemetry.Terrain.TerrainResourceSuccessfulDraws = Resolved.Observations.ResourceSuccessfulDraws;
		Telemetry.Terrain.TerrainResourceRejectedDraws = Resolved.Observations.ResourceRejectedDraws;
		Telemetry.Terrain.PreparedTerrainBatches = Resolved.Observations.PreparedBatches;
		Telemetry.Terrain.TerrainBatchChunks = Resolved.Observations.BatchChunks;
		Telemetry.Terrain.TerrainInstances = Resolved.Observations.InstanceCount;
		Telemetry.Terrain.TerrainInstanceBytes = Resolved.Observations.InstanceBytes;
		Telemetry.Terrain.TerrainInstanceAllocations = Resolved.Observations.InstanceAllocations;
		Telemetry.Terrain.TerrainResourceAttemptedBatches = Resolved.Observations.ResourceAttemptedBatches;
		Telemetry.Terrain.TerrainResourceSuccessfulBatches = Resolved.Observations.ResourceSuccessfulBatches;
		Telemetry.Terrain.TerrainResourceRejectedBatches = Resolved.Observations.ResourceRejectedBatches;
		Telemetry.Terrain.TerrainSubmittedLogicalPatches = Resolved.Observations.SubmittedLogicalPatches;
		Telemetry.Terrain.TerrainScalarTranslucentDraws = Resolved.Observations.ScalarTranslucentDraws;
		Telemetry.Terrain.TerrainLogicalPreparationNanoseconds = Terrain.LogicalPreparationNanoseconds;
		Telemetry.Terrain.TerrainBatchConstructionNanoseconds = Terrain.BatchConstructionNanoseconds;
		Telemetry.Terrain.TerrainResourcePreparationNanoseconds = Resolved.Observations.ResourcePreparationNanoseconds;
		Telemetry.Terrain.TerrainHeightPreparationNanoseconds = Resolved.Observations.HeightPreparationNanoseconds;
		Telemetry.Terrain.TerrainTopologyPreparationNanoseconds = Resolved.Observations.TopologyPreparationNanoseconds;
		Telemetry.Terrain.TerrainShaderPreparationNanoseconds = Resolved.Observations.ShaderPreparationNanoseconds;
		Telemetry.Terrain.TerrainPipelinePreparationNanoseconds = Resolved.Observations.PipelinePreparationNanoseconds;
		Telemetry.Terrain.TerrainDynamicAllocationNanoseconds = Resolved.Observations.DynamicAllocationNanoseconds;
		Telemetry.Terrain.TerrainCommandRecordingNanoseconds = Resolved.Observations.CommandRecordingNanoseconds;
		Telemetry.Terrain.TerrainAttemptedDraws = Resolved.Observations.AttemptedDraws;
		Telemetry.Terrain.TerrainSuccessfulDraws = Resolved.Observations.SuccessfulDraws;
		Telemetry.Terrain.TerrainRejectedDraws = Resolved.Observations.RejectedDraws;
		Telemetry.Terrain.TerrainHeightUploadBytes = Resolved.Observations.HeightUploadBytes;
		Telemetry.Terrain.TerrainHeightUploads = Resolved.Observations.HeightUploads;
		Telemetry.Terrain.TerrainHeightReuses = Resolved.Observations.HeightReuses;
		Telemetry.Terrain.TerrainTopologyCreations = Resolved.Observations.TopologyCreations;
		Telemetry.Terrain.TerrainTopologyReuses = Resolved.Observations.TopologyReuses;
		Telemetry.Terrain.TerrainTopologyBytes = Resolved.Observations.TopologyBytes;
		Telemetry.Terrain.TerrainShaderLookups = Resolved.Observations.ShaderLookups;
		Telemetry.Terrain.TerrainShaderCreations = Resolved.Observations.ShaderCreations;
		Telemetry.Terrain.TerrainShaderReuses = Resolved.Observations.ShaderReuses;
		Telemetry.Terrain.TerrainPipelineLookups = Resolved.Observations.PipelineLookups;
		Telemetry.Terrain.TerrainPipelineCreations = Resolved.Observations.PipelineCreations;
		Telemetry.Terrain.TerrainPipelineReuses = Resolved.Observations.PipelineReuses;
	}
} // namespace Durin
