#include "Rendering/SplineMeshSceneProxy.h"

#include "Math/Operations.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	FSplineMeshSceneProxy::FSplineMeshSceneProxy(
		const FStaticMeshRenderData* InRenderData,
		std::vector<FMaterialRenderProxyRef> InMaterialProxies,
		uint64 InMaterialComponentRevision,
		FSplineMeshRenderDynamicData InDynamicData)
		: RenderData(InRenderData), Materials(std::move(InMaterialProxies)),
		  MaterialComponentRevision(InMaterialComponentRevision), DynamicData(std::move(InDynamicData))
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
		if (Update.ComponentRevision <= MaterialComponentRevision
			|| Update.SlotIndex >= Materials.size()) return false;
		Materials[Update.SlotIndex] = Update.MaterialProxy;
		MaterialComponentRevision = Update.ComponentRevision;
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
		++AcceptedDynamicUpdateCount;
		return true;
	}
}
