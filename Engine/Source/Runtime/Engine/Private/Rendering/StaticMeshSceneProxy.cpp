#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/StaticMeshBatchCollection.h"

#include "StaticMesh/StaticMeshResources.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	FStaticMeshSceneProxy::FStaticMeshSceneProxy(
		const FStaticMeshRenderData* InRenderData,
		std::vector<FMaterialRenderProxyRef> InMaterialProxies, FStaticMeshRenderInstances InInstances)
		: RenderData(InRenderData)
		, Materials(std::move(InMaterialProxies))
	{
		requiref(SetRenderInstances(std::move(InInstances)), "StaticMesh proxy requires valid render instances.");
	}

	auto FStaticMeshSceneProxy::SetRenderInstances(FStaticMeshRenderInstances InInstances) -> bool
	{
		if (InInstances && !ValidateStaticMeshRenderInstances(*InInstances)) return false;
		Instances = std::move(InInstances);
		return true;
	}

	auto FStaticMeshSceneProxy::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData;
	}

	auto FStaticMeshSceneProxy::CaptureLODSelection_RenderThread() const -> std::optional<FMeshLODSelectionSnapshot>
	{
		CheckRenderingThread();
		return CaptureStaticMeshLODSelection(RenderData ? std::span<const FStaticMeshLODResources>(RenderData->LODResources)
			: std::span<const FStaticMeshLODResources>{});
	}

	auto FStaticMeshSceneProxy::GetLocalBounds() const -> FBox
	{
		if (!RenderData) return {};
		if (!Instances) return RenderData->LocalBounds;
		FBox Bounds;
		for (const auto& Instance : *Instances)
		{
			const auto Transformed = TransformPrimitiveBounds(RenderData->LocalBounds, Instance.LocalToPrimitive);
			if (Transformed.bIsValid) { Bounds.AddPoint(Transformed.Min); Bounds.AddPoint(Transformed.Max); }
		}
		return Bounds;
	}

	auto FStaticMeshSceneProxy::ResolveMaterialRenderData_RenderThread(
		uint32 SlotIndex) const -> const FMaterialRenderData&
	{
		const FMaterialRenderProxyRef& MaterialProxy = GetMaterialRenderProxy(SlotIndex);
		if (MaterialProxy) return MaterialProxy->Resolve_RenderThread();
		RecordMaterialFallbackReason(EMaterialFallbackReason::MissingProxy);
		return GetErrorMaterialRenderData();
	}

	auto FStaticMeshSceneProxy::GetMaterialRenderProxy(uint32 SlotIndex) const
		-> const FMaterialRenderProxyRef&
	{
		static const FMaterialRenderProxyRef EmptyProxy;
		return SlotIndex < Materials.size() ? Materials[SlotIndex] : EmptyProxy;
	}

	auto FStaticMeshSceneProxy::UpdateMaterialBinding_RenderThread(
		const FMaterialRenderProxyBindingUpdate& Update) -> bool
	{
		CheckRenderingThread();
		if (Update.SlotIndex >= Materials.size()) return false;
		Materials[Update.SlotIndex] = Update.MaterialProxy;
		RecordMaterialBindingUpdate();
		return true;
	}

	auto FStaticMeshSceneProxy::CollectMeshBatches(const FMeshCollectionContext& Context,
		FMeshBatchCollector& Collector) const -> void
	{
		if (!Instances) return CollectStaticMeshAssetBatches(*this, RenderData, Context, Collector);
		if (!RenderData) return;
		for (const auto& Instance : *Instances)
		{
			auto InstanceContext = Context;
			InstanceContext.BatchId = Instance.Id;
			InstanceContext.CustomInstanceData = Instance.CustomData;
			InstanceContext.LocalToWorld = Context.LocalToWorld * Instance.LocalToPrimitive;
			InstanceContext.WorldBounds = TransformPrimitiveBounds(RenderData->LocalBounds, InstanceContext.LocalToWorld);
			CollectStaticMeshAssetBatches(*this, RenderData, InstanceContext, Collector);
		}
	}
}
