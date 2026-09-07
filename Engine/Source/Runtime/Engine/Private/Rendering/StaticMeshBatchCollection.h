#pragma once

#include "Rendering/MeshBatch.h"
#include "Rendering/StaticMeshBatchBinding.h"
#include "StaticMesh/StaticMeshLODSelection.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	// One asset-specific selection/material algorithm shared by both providers.
	template <typename TProxy, typename TBinding>
	auto CollectStaticMeshAssetBatches(const TProxy& Proxy,
		const FStaticMeshRenderData* RenderData,
		const FMeshCollectionContext& Context, FMeshBatchCollector& Collector,
		std::shared_ptr<TBinding> Binding) -> void
	{
		CheckRenderingThread();
		if (!RenderData || RenderData->LODResources.empty()
			|| RenderData->LODVertexFactories.size() != RenderData->LODResources.size())
		{
			Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
			return;
		}
		const uint32 RequestedLOD = Context.bForceLOD0 ? 0u
			: SelectStaticMeshLOD(Context.NormalizedScreenSize, RenderData->LODResources);
		const uint32 SelectedLOD = ResolveAvailableStaticMeshLOD(
			RequestedLOD, RenderData->LODResources);
		if (SelectedLOD == InvalidStaticMeshLODIndex)
		{
			Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
			return;
		}
		const auto& LOD = RenderData->LODResources[SelectedLOD];
		const auto& VertexFactory = RenderData->LODVertexFactories[SelectedLOD].VertexFactory;
		Binding->Declaration = VertexFactory.GetDeclaration();
		Binding->DeclarationElements = VertexFactory.GetDeclarationElements();
		Binding->Streams = VertexFactory.GetStreams();
		Binding->NumVertices = LOD.GetNumVertices();
		FMeshBatch Batch;
		Batch.PrimitiveId = Context.PrimitiveId;
		Batch.LocalToWorld = Context.LocalToWorld;
		Batch.WorldBounds = Context.WorldBounds;
		Batch.FactoryKey = Binding->GetFactoryKey();
		Batch.LayoutKey = Binding->GetLayoutKey();
		Batch.Binding = std::move(Binding);
		Batch.RequestedLOD = RequestedLOD;
		Batch.SelectedLOD = SelectedLOD;
		Batch.LODCount = static_cast<uint32>(RenderData->LODResources.size());
		if constexpr (std::same_as<TBinding, FSplineMeshBatchBinding>)
		{
			Batch.AcceptedDynamicUpdates = Proxy.GetAcceptedDynamicUpdateCount();
			Batch.RetainedDeformationBytes = sizeof(FSplineMeshRenderDynamicData);
		}
		Batch.Elements.reserve(LOD.Sections.size());
		const auto& Position = LOD.VertexBuffers.PositionVertexBuffer;
		for (uint32 SectionIndex = 0; SectionIndex < LOD.Sections.size(); ++SectionIndex)
		{
			const auto& Section = LOD.Sections[SectionIndex];
			if (Section.IndexCount == 0 || static_cast<uint64>(Section.FirstIndex)
				+ Section.IndexCount > LOD.IndexBuffer.GetIndices().size()) continue;
			FMeshBatchElement Element;
			Element.ElementId = SectionIndex;
			Element.LocalBounds = Section.LocalBounds;
			Element.MaterialSlotDiagnostic = Section.MaterialSlotIndex;
			Element.Draw.ElementCount = Section.IndexCount;
			Element.Draw.FirstElement = Section.FirstIndex;
			Element.Draw.MinVertexIndex = Section.MinVertexIndex;
			Element.Draw.MaxVertexIndex = Section.MaxVertexIndex;
			Element.Vertices.Buffer = Position.GetRHI();
			Element.Vertices.Range = {static_cast<uint64>(Position.GetNumVertices()) * Position.GetStride(),
				0, Position.GetStride(), Position.GetStride()};
			Element.Indices.Buffer = LOD.IndexBuffer.GetRHI();
			Element.Indices.Range = {static_cast<uint64>(LOD.GetNumIndices()) * sizeof(uint32),
				0, sizeof(uint32), sizeof(uint32)};
			Element.Material = Proxy.ResolveMaterialRenderData_RenderThread(Section.MaterialSlotIndex);
			Batch.Elements.push_back(std::move(Element));
		}
		Collector.Add(std::move(Batch));
	}
}
