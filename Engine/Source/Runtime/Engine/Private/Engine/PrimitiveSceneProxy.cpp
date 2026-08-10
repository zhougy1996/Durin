#include "Engine/FPrimitiveSceneProxy.h"

#include "StaticMesh/StaticMeshResources.h"
#include "SkeletalMesh/SkeletalMeshResources.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	FStaticMeshSceneProxy::FStaticMeshSceneProxy(
		const FStaticMeshRenderData* InRenderData,
		std::vector<FMaterialRenderProxyRef> InMaterialProxies,
		uint64 InMaterialComponentRevision)
		: RenderData(InRenderData)
		, Materials(std::move(InMaterialProxies))
		, MaterialComponentRevision(InMaterialComponentRevision)
	{
	}

	auto FStaticMeshSceneProxy::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData;
	}

	auto FStaticMeshSceneProxy::GetLocalBounds() const -> FBox
	{
		return RenderData != nullptr ? RenderData->LocalBounds : FBox{};
	}

	auto FStaticMeshSceneProxy::ResolveMaterialRenderData_RenderThread(
		uint32 SlotIndex) const -> const FMaterialRenderData&
	{
		const FMaterialRenderProxyRef& MaterialProxy =
			GetMaterialRenderProxy(SlotIndex);
		if (MaterialProxy) return MaterialProxy->Resolve_RenderThread();
		RecordMaterialFallbackReason(
			EMaterialFallbackReason::MissingProxy);
		return GetErrorMaterialRenderData();
	}

	auto FStaticMeshSceneProxy::GetMaterialRenderProxy(uint32 SlotIndex) const
		-> const FMaterialRenderProxyRef&
	{
		static const FMaterialRenderProxyRef EmptyProxy;
		return SlotIndex < Materials.size() ? Materials[SlotIndex] : EmptyProxy;
	}

	auto FStaticMeshSceneProxy::UpdateMaterialRenderProxyBinding(
		const FMaterialRenderProxyBindingUpdate& Update) -> void
	{
		CheckRenderingThread();
		if (Update.ComponentRevision <= MaterialComponentRevision) return;
		if (Update.SlotIndex >= Materials.size()) return;
		Materials[Update.SlotIndex] = Update.MaterialProxy;
		MaterialComponentRevision = Update.ComponentRevision;
		RecordMaterialBindingUpdate();
	}

	auto FStaticMeshSceneProxy::UpdateMaterialBinding_RenderThread(
		const FMaterialRenderProxyBindingUpdate& Update) -> bool
	{
		UpdateMaterialRenderProxyBinding(Update);
		return true;
	}

	FSkeletalMeshSceneProxy::FSkeletalMeshSceneProxy(
		const FSkeletalMeshRenderData* InRenderData,
		std::vector<FMaterialRenderProxyRef> InMaterialProxies,
		uint64 InMaterialComponentRevision,
		std::shared_ptr<const FSkeletalPosePalette> InPose)
		: RenderData(InRenderData), Materials(std::move(InMaterialProxies)),
		  MaterialComponentRevision(InMaterialComponentRevision), Pose(std::move(InPose))
	{
	}

	auto FSkeletalMeshSceneProxy::GetLocalBounds() const -> FBox
	{
		return Pose ? Pose->LocalBounds : FBox{};
	}

	auto FSkeletalMeshSceneProxy::GetMaterialRenderProxy(uint32 SlotIndex) const
		-> const FMaterialRenderProxyRef&
	{
		static const FMaterialRenderProxyRef EmptyProxy;
		return SlotIndex < Materials.size() ? Materials[SlotIndex] : EmptyProxy;
	}

	auto FSkeletalMeshSceneProxy::ResolveMaterialRenderData_RenderThread(uint32 SlotIndex) const
		-> const FMaterialRenderData&
	{
		const FMaterialRenderProxyRef& Proxy = GetMaterialRenderProxy(SlotIndex);
		if (Proxy) return Proxy->Resolve_RenderThread();
		RecordMaterialFallbackReason(EMaterialFallbackReason::MissingProxy);
		return GetErrorMaterialRenderData();
	}

	auto FSkeletalMeshSceneProxy::UpdateMaterialBinding_RenderThread(
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

	auto FSkeletalMeshSceneProxy::UpdateDynamicData_RenderThread(
		std::shared_ptr<const FSkeletalPosePalette> InPose) -> bool
	{
		CheckRenderingThread();
		if (!InPose || !InPose->LocalBounds.bIsValid) return false;
		Pose = std::move(InPose);
		return true;
	}

	FTextureCubePreviewSceneProxy::FTextureCubePreviewSceneProxy(
		const FStaticMeshRenderData* InRenderData,
		FRHITextureReferenceRef InTextureReference)
		: RenderData(InRenderData)
		, TextureReference(std::move(InTextureReference))
	{
	}

	auto FTextureCubePreviewSceneProxy::GetLocalBounds() const -> FBox
	{
		return RenderData != nullptr ? RenderData->LocalBounds : FBox{};
	}
}
