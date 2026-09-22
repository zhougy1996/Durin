#pragma once

#include "Rendering/MeshBatch.h"
#include "Rendering/MeshGeometryRecord.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "StaticMesh/StaticMeshLODSelection.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	// One asset-specific selection/material algorithm shared by both providers.
	template <typename TProxy>
	auto CollectStaticMeshAssetBatches(const TProxy& Proxy,
		const FStaticMeshRenderData* RenderData,
		const FMeshCollectionContext& Context, FMeshBatchCollector& Collector) -> void
	{
		CheckRenderingThread();
		if (!RenderData || RenderData->LODResources.empty()
			|| RenderData->LODVertexFactories.size() != RenderData->LODResources.size())
		{
			Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
			return;
		}
		const uint32 RequestedLOD = Context.PreparedLOD ? Context.PreparedLOD->Requested
			: Context.bForceLOD0 ? 0u : SelectStaticMeshLOD(Context.NormalizedScreenSize, RenderData->LODResources);
		const uint32 SelectedLOD = Context.PreparedLOD ? Context.PreparedLOD->Selected
			: ResolveAvailableStaticMeshLOD(RequestedLOD, RenderData->LODResources);
		if (RequestedLOD >= RenderData->LODResources.size() || SelectedLOD >= RenderData->LODResources.size()
			|| !RenderData->LODResources[SelectedLOD].bReadyForRendering)
		{
			Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
			return;
		}
		const auto& LOD = RenderData->LODResources[SelectedLOD];
		auto Record = LOD.GeometryRecord;
		if (!Record)
		{
			Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
			return;
		}
		if constexpr (std::same_as<TProxy, FSplineMeshSceneProxy>)
		{
			Record = Proxy.ResolveGeometryRecord_RenderThread(SelectedLOD, *Record);
			if (!Record)
			{
				Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
				return;
			}
		}
		const auto& Inputs = Record->GetBinding();
		FMeshBatch Batch;
		Batch.PrimitiveId = Context.PrimitiveId;
		Batch.LocalToWorld = Context.LocalToWorld;
		Batch.WorldBounds = Context.WorldBounds;
		Batch.FactoryKey = Inputs->GetFactoryKey();
		Batch.LayoutKey = Inputs->GetLayoutKey();
		Batch.Binding = Inputs;
		Batch.GeometryRecord = Record;
		Batch.RequestedLOD = RequestedLOD;
		Batch.SelectedLOD = SelectedLOD;
		Batch.LODCount = static_cast<uint32>(RenderData->LODResources.size());
		if constexpr (std::same_as<TProxy, FSplineMeshSceneProxy>)
		{
			Batch.AcceptedDynamicUpdates = Proxy.GetAcceptedDynamicUpdateCount();
			Batch.RetainedDeformationBytes = sizeof(FSplineMeshRenderDynamicData);
		}
		Batch.PublishedMaterials.reserve(Record->GetElements().size());
		for (const auto& Geometry : Record->GetElements())
		{
			Batch.PublishedMaterials.push_back(Proxy.ResolveMaterialRenderData_RenderThread(Geometry.MaterialSlotDiagnostic));
		}
		Collector.Add(std::move(Batch));
	}
}
