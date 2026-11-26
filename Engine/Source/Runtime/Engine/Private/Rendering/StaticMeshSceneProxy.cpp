#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/StaticMeshBatchCollection.h"
#include "Materials/MaterialShaderMap.h"
#include "InstanceUniformShaderParameters.h"

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
		InstanceSceneData.reset();
		if (Instances)
		{
			auto Snapshot = std::make_shared<FInstanceSceneDataBuffers>();
			Snapshot->Instances.reserve(Instances->size());
			for (const auto& Instance : *Instances)
				Snapshot->Instances.push_back({{Instance.Id}, Instance.LocalToPrimitive,
					RenderData ? RenderData->LocalBounds : FBox{}, Instance.CustomData});
			InstanceSceneData = std::move(Snapshot);
		}
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

	auto FStaticMeshBatchCaptureContext::ResolveMaterial(const FMaterialRenderProxyRef& Proxy) -> const FMaterialRenderData&
	{
		CheckRenderingThread();
		const auto Key = Proxy.GetReference();
		if (const auto Found = Materials.find(Key); Found != Materials.end()) return Found->second;
		return Materials.emplace(Key, QueryMeshMaterial_RenderThread(Proxy)).first->second;
	}

	auto FStaticMeshSceneProxy::CanBeOccluded() const -> bool
	{
		CheckRenderingThread();
		if (typeid(*this) != typeid(FStaticMeshSceneProxy) || !RenderData) return false;
		return RenderData->HasValidLODLayout() && std::ranges::all_of(RenderData->GetReferencedMaterialSlots_RenderThread(), [&](uint32 MaterialSlotIndex) {
			const auto& MaterialProxy = GetMaterialRenderProxy(MaterialSlotIndex);
			return MaterialProxy && MaterialProxy->HasStableOpaqueDepth_RenderThread();
		});
	}

	namespace
	{
		auto SupportsUnifiedShadowMesh(const FMaterialRenderProxyRef& MaterialProxy, ERHIFeatureLevel FeatureLevel) -> bool
		{
			if (!MaterialProxy || !MaterialProxy->HasStableOpaqueDepth_RenderThread()) return false;
			const auto& Material = *MaterialProxy->GetMaterialNoFallback();
			const auto& ShaderMap = Material.CompiledProgram->GetShaderMap();
			constexpr uint8 LocalVertexFactoryUsage = 1;
			constexpr uint8 ShadowPassUsage = 4;
			return Material.CompiledProgram->FeatureLevel == FeatureLevel
				&& ShaderMap.Domain == EMaterialDomain::Surface
				&& !Material.PlanningPassIdentity.bTwoSided
				&& Material.PlanningPassIdentity.ShaderMap.ShadingModel != EMaterialShadingModel::SingleLayerWater
				&& (ShaderMap.StaticProperties.GeometryUsage & LocalVertexFactoryUsage) != 0
				&& (ShaderMap.StaticProperties.PassUsage & ShadowPassUsage) != 0;
		}

		auto SetStaticMeshBatchGeometry(FMeshBatch& MeshBatch, const FMeshGeometryRecord::FRef& Geometry,
			uint32 SegmentIndex, uint32 LODIndex, uint32 LODCount) -> void
		{
			MeshBatch.GeometryRecord = Geometry;
			MeshBatch.Binding = Geometry->GetBinding();
			MeshBatch.FactoryKey = MeshBatch.Binding->GetFactoryKey();
			MeshBatch.LayoutKey = MeshBatch.Binding->GetLayoutKey();
			MeshBatch.SegmentIndex = SegmentIndex;
			MeshBatch.RequestedLOD = MeshBatch.SelectedLOD = LODIndex;
			MeshBatch.LODCount = LODCount;
			MeshBatch.PrimitiveIdMode = EPrimitiveIdMode::PrimID_FromPrimitiveSceneInfo;
		}
	}

	auto FStaticMeshSceneProxy::GetMeshElement(uint32 LODIndex, uint32 SectionIndex, FMeshBatch& OutMeshBatch) const -> void
	{
		const auto& Geometry = RenderData->LODResources[LODIndex].GeometryRecord;
		SetStaticMeshBatchGeometry(OutMeshBatch, Geometry, SectionIndex, LODIndex, static_cast<uint32>(RenderData->LODResources.size()));
		OutMeshBatch.MaterialRenderProxy = GetMaterialRenderProxy(Geometry->GetElements()[SectionIndex].MaterialSlotIndex);
		OutMeshBatch.CastShadow = GetRenderParticipation().bCastShadow;
	}

	auto FStaticMeshSceneProxy::GetShadowMeshElement(uint32 LODIndex, FMeshBatch& OutMeshBatch) const -> bool
	{
		const auto& LOD = RenderData->LODResources[LODIndex];
		if (!GetRenderParticipation().bCastShadow || !LOD.ShadowGeometryRecord) return false;
		for (const auto& Section : LOD.GeometryRecord->GetElements())
		{
			if (!SupportsUnifiedShadowMesh(GetMaterialRenderProxy(Section.MaterialSlotIndex), GetFeatureLevel()))
				return false;
		}
		SetStaticMeshBatchGeometry(OutMeshBatch, LOD.ShadowGeometryRecord, 0, LODIndex, static_cast<uint32>(RenderData->LODResources.size()));
		OutMeshBatch.MaterialRenderProxy = GetMaterialRenderProxy(LOD.ShadowGeometryRecord->GetElements().front().MaterialSlotIndex);
		OutMeshBatch.bUseForMaterial = false;
		OutMeshBatch.CastShadow = true;
		return true;
	}

	auto FStaticMeshSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) const -> void
	{
		CheckRenderingThread();
		if (Instances || !RenderData || !RenderData->HasValidLODLayout()) return;
		for (uint32 LODIndex = 0; LODIndex < RenderData->LODResources.size(); ++LODIndex)
		{
			const auto& LOD = RenderData->LODResources[LODIndex];
			if (!LOD.IsReadyForRendering() || !LOD.GeometryRecord) continue;
			FMeshBatch ShadowMeshBatch;
			const bool bUseUnifiedMeshForShadow = GetShadowMeshElement(LODIndex, ShadowMeshBatch);
			if (bUseUnifiedMeshForShadow) PDI->DrawMesh(ShadowMeshBatch, LOD.ScreenSize);

			for (uint32 SectionIndex = 0; SectionIndex < LOD.GeometryRecord->GetElements().size(); ++SectionIndex)
			{
				FMeshBatch MeshBatch;
				GetMeshElement(LODIndex, SectionIndex, MeshBatch);
				MeshBatch.CastShadow &= !bUseUnifiedMeshForShadow;
				PDI->DrawMesh(MeshBatch, LOD.ScreenSize);
			}
		}
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
			InstanceContext.InstanceSceneDataId = {Instance.Id};
			InstanceContext.CustomInstanceData = Instance.CustomData;
			InstanceContext.LocalToWorld = Context.LocalToWorld * Instance.LocalToPrimitive;
			InstanceContext.WorldBounds = TransformPrimitiveBounds(RenderData->LocalBounds, InstanceContext.LocalToWorld);
			CollectStaticMeshAssetBatches(*this, RenderData, InstanceContext, Collector);
		}
	}
}
