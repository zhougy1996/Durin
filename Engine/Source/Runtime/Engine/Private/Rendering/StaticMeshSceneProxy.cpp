#include "Rendering/StaticMeshSceneProxy.h"
#include "Rendering/StaticMeshBatchCollection.h"

#include "StaticMesh/StaticMeshResources.h"
#include "Threading/RunnableThread.h"
#include "RenderPipelineCreation.h"

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
		StaticBatches.reset();
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
		if (MaterialProxy)
		{
			if (const auto* Material = MaterialProxy->GetMaterialNoFallback()) return *Material;
			if (MaterialProxy->IsCompiling()) FRenderPipelinePreparationBatch::MarkResourcePending();
			return MaterialProxy->GetMaterialWithFallback();
		}
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
		StaticBatches.reset();
		RecordMaterialBindingUpdate();
		return true;
	}

	auto FStaticMeshBatchCaptureContext::ResolveMaterial(const FMaterialRenderProxyRef& Proxy) -> const FMaterialRenderData&
	{
		CheckRenderingThread();
		const auto Key = Proxy.GetReference();
		if (const auto Found = Materials.find(Key); Found != Materials.end()) return Found->second;
		if (!Proxy) RecordMaterialFallbackReason(EMaterialFallbackReason::MissingProxy);
		if (Proxy && !Proxy->GetMaterialNoFallback() && Proxy->IsCompiling())
			FRenderPipelinePreparationBatch::MarkResourcePending();
		return Materials.emplace(Key, Proxy ? Proxy->GetMaterialWithFallback() : GetErrorMaterialRenderData()).first->second;
	}

	auto FStaticMeshSceneProxy::CaptureStaticMeshBatches_RenderThread(FStaticMeshBatchCaptureContext& Context) const
		-> FStaticMeshBatchPublication::FRef
	{
		CheckRenderingThread();
		if (const auto Found = Context.Publications.find(this); Found != Context.Publications.end()) return Found->second;
		if (Instances || !RenderData) return Context.Publications.emplace(this, nullptr).first->second;
		const auto Geometry = RenderData->CaptureBatchGeometry_RenderThread();
		if (!Geometry->bValidLayout) return Context.Publications.emplace(this, nullptr).first->second;
		bool bCurrent = StaticBatches && StaticBatches->Geometry == Geometry
			&& StaticBatches->MaterialRecords.size() == Materials.size();
		for (size_t Index = 0; Index < Materials.size(); ++Index)
		{
			const auto& Material = Context.ResolveMaterial(Materials[Index]);
			if (bCurrent && StaticBatches->MaterialRecords[Index] != Material.Representation.GetRecordId()) bCurrent = false;
		}
		if (bCurrent && StaticBatches->bUsesFallbackMaterial)
			bCurrent = StaticBatches->FallbackMaterialRecord == Context.ResolveMaterial({}).Representation.GetRecordId();
		if (!bCurrent)
		{
			auto Candidate = std::make_shared<FStaticMeshBatchPublication>();
			Candidate->Geometry = Geometry;
			Candidate->MaterialRecords.reserve(Materials.size());
			for (const auto& Material : Materials)
				Candidate->MaterialRecords.push_back(Context.ResolveMaterial(Material).Representation.GetRecordId());
			Candidate->LODs.reserve(Geometry->Geometry.size());
			for (const auto& Record : Geometry->Geometry)
			{
				if (!Record) { Candidate->LODs.push_back(nullptr); continue; }
				auto Inputs = std::make_shared<FStaticMeshBatchInputs>();
				Inputs->Geometry = Record;
				Inputs->Materials.reserve(Record->GetElements().size());
				for (const auto& Element : Record->GetElements())
				{
					const auto& Proxy = GetMaterialRenderProxy(Element.MaterialSlotDiagnostic);
					const auto& Material = Context.ResolveMaterial(Proxy);
					Inputs->Materials.push_back(Material);
					if (!Proxy)
					{
						Candidate->bUsesFallbackMaterial = true;
						Candidate->FallbackMaterialRecord = Material.Representation.GetRecordId();
					}
				}
				Candidate->LODs.push_back(std::move(Inputs));
			}
			StaticBatches = std::move(Candidate);
		}
		return Context.Publications.emplace(this, StaticBatches).first->second;
	}

	auto CollectPublishedStaticMeshBatch(const FStaticMeshBatchPublication& Publication,
		const FMeshCollectionContext& Context, FMeshBatchCollector& Collector) -> void
	{
		CheckRenderingThread();
		if (!Publication.Geometry || !Publication.Geometry->bValidLayout)
		{
			Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
			return;
		}
		const auto& LODs = Publication.Geometry->LODs;
		const uint32 Requested = Context.PreparedLOD ? Context.PreparedLOD->Requested
			: Context.bForceLOD0 ? 0u : SelectStaticMeshLOD(Context.NormalizedScreenSize, LODs);
		const uint32 Selected = Context.PreparedLOD ? Context.PreparedLOD->Selected : ResolveAvailableStaticMeshLOD(Requested, LODs);
		if (Requested >= LODs.size() || Selected >= LODs.size() || !LODs[Selected].bReadyForRendering
			|| Selected >= Publication.LODs.size() || !Publication.LODs[Selected] || !Publication.LODs[Selected]->Geometry)
		{
			Collector.RecordOutcome(EGeometrySubmissionOutcome::ResourceFailure);
			return;
		}
		FMeshBatch Batch;
		Batch.StaticInputs = Publication.LODs[Selected];
		Batch.GeometryRecord = Batch.StaticInputs->Geometry;
		Batch.Binding = Batch.GeometryRecord->GetBinding();
		Batch.FactoryKey = Batch.Binding->GetFactoryKey();
		Batch.LayoutKey = Batch.Binding->GetLayoutKey();
		Batch.PrimitiveId = Context.PrimitiveId;
		Batch.BatchId = Context.BatchId;
		Batch.LocalToWorld = Context.LocalToWorld;
		Batch.WorldBounds = Context.WorldBounds;
		Batch.CustomPrimitiveData = Context.CustomPrimitiveData;
		Batch.CustomInstanceData = Context.CustomInstanceData;
		Batch.RequestedLOD = Requested;
		Batch.SelectedLOD = Selected;
		Batch.LODCount = static_cast<uint32>(LODs.size());
		Collector.Add(std::move(Batch));
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
