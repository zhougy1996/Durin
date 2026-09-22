#include "Rendering/SplineMeshSceneProxy.h"
#include "Rendering/StaticMeshBatchCollection.h"

#include "Math/Operations.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	FSplineMeshSceneProxy::FSplineMeshSceneProxy(
		const FStaticMeshRenderData* InRenderData,
		std::vector<FMaterialRenderProxyRef> InMaterialProxies,
		FSplineMeshRenderDynamicData InDynamicData)
		: RenderData(InRenderData), Materials(std::move(InMaterialProxies)),
		  DynamicData(std::move(InDynamicData))
	{
	}

	auto FSplineMeshSceneProxy::GetMaterialRenderProxy(uint32 SlotIndex) const
		-> const FMaterialRenderProxyRef&
	{
		static const FMaterialRenderProxyRef EmptyProxy;
		return SlotIndex < Materials.size() ? Materials[SlotIndex] : EmptyProxy;
	}

	auto FSplineMeshSceneProxy::ResolveMaterialRenderData_RenderThread(uint32 SlotIndex) const
		-> const FMaterialRenderData&
	{
		const FMaterialRenderProxyRef& Proxy = GetMaterialRenderProxy(SlotIndex);
		if (Proxy) return Proxy->Resolve_RenderThread();
		RecordMaterialFallbackReason(EMaterialFallbackReason::MissingProxy);
		return GetErrorMaterialRenderData();
	}

	auto FSplineMeshSceneProxy::UpdateMaterialBinding_RenderThread(
		const FMaterialRenderProxyBindingUpdate& Update) -> bool
	{
		CheckRenderingThread();
		if (Update.SlotIndex >= Materials.size()) return false;
		Materials[Update.SlotIndex] = Update.MaterialProxy;
		RecordMaterialBindingUpdate();
		return true;
	}

	auto FSplineMeshSceneProxy::UpdateDynamicData_RenderThread(
		FSplineMeshRenderDynamicData InDynamicData) -> bool
	{
		CheckRenderingThread();
		if (InDynamicData.Revision <= DynamicData.Revision || !InDynamicData.LocalBounds.bIsValid
			|| !Math::IsFinite(InDynamicData.LocalBounds.Min)
			|| !Math::IsFinite(InDynamicData.LocalBounds.Max)) return false;
		DynamicData = std::move(InDynamicData);
		GeometryBindings.clear();
		++AcceptedDynamicUpdateCount;
		return true;
	}

	auto FSplineMeshSceneProxy::CollectMeshBatches(const FMeshCollectionContext& Context,
		FMeshBatchCollector& Collector) const -> void
	{
		CollectStaticMeshAssetBatches(*this, RenderData, Context, Collector);
	}

	auto FSplineMeshSceneProxy::CaptureLODSelection_RenderThread() const -> std::optional<FMeshLODSelectionSnapshot>
	{
		CheckRenderingThread();
		return CaptureStaticMeshLODSelection(RenderData ? std::span<const FStaticMeshLODResources>(RenderData->LODResources)
			: std::span<const FStaticMeshLODResources>{});
	}

	auto FSplineMeshSceneProxy::ResolveGeometryRecord_RenderThread(uint32 LODIndex,
		const FMeshGeometryRecord& Geometry) const
		-> std::shared_ptr<const FMeshGeometryRecord>
	{
		CheckRenderingThread();
		check(RenderData && LODIndex < RenderData->LODResources.size());
		GeometryBindings.resize(RenderData->LODResources.size());
		auto& Cached = GeometryBindings[LODIndex];
		if (Cached.GeometryRecordId == Geometry.GetRecordId() && Cached.Record)
			return Cached.Record;
		auto Binding = std::make_unique<FSplineMeshBatchBinding>();
		Binding->Declaration = Geometry.GetBinding()->Declaration;
		Binding->DeclarationElements = Geometry.GetBinding()->DeclarationElements;
		Binding->Streams = Geometry.GetBinding()->Streams;
		Binding->NumVertices = Geometry.GetBinding()->NumVertices;
		Binding->DynamicData = DynamicData;
		auto Record = Geometry.WithBinding(std::move(Binding));
		if (!Record) return {};
		Cached = {Geometry.GetRecordId(), std::move(*Record)};
		return Cached.Record;
	}
}
