#include "Renderers/SceneRenderTelemetry.h"

#include "Renderers/StaticMeshRenderPreparation.h"

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

		Result.SplineMesh.Triangles = Telemetry.SplineMesh.PreparedSplineMeshTriangles;
		Result.StaticMesh.Triangles = Telemetry.StaticMesh.PreparedStaticMeshTriangles
									  - std::min(Telemetry.StaticMesh.PreparedStaticMeshTriangles, Telemetry.SplineMesh.PreparedSplineMeshTriangles);
		Result.Summary.Triangles = AddSaturated(
			Result.StaticMesh.Triangles, Result.SplineMesh.Triangles
		);
		Result.Shadow.Triangles = Telemetry.DirectionalShadow.ShadowPreparedTriangles;

		Result.StaticMesh.DrawCalls = AddSaturated(
			Telemetry.StaticMesh.StaticMeshSuccessfulDraws,
			AddSaturated(
				Telemetry.GBuffer.GBufferStaticMeshSuccessfulDraws,
				Telemetry.GBuffer.GBufferSplineMeshSuccessfulDraws));
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


} // namespace Durin
