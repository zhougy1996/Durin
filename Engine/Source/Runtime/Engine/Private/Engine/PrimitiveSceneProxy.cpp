#include "Engine/PrimitiveSceneProxy.h"

#include "Threading/RunnableThread.h"

namespace Durin
{
	auto PrimitiveSceneProxy::SetTransform(FRHICommandListBase& RHICmdList, const FMatrix& InLocalToWorld, FVector3 InActorPosition) -> void
	{
		LocalToWorld_ = InLocalToWorld;
		ActorPosition_ = InActorPosition;
	}

	auto PrimitiveSceneProxy::GetLocalToWorld() const -> const FMatrix&
	{
		return LocalToWorld_;
	}

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

	auto FStaticMeshSceneProxy::ResolveMaterialRenderData_RenderThread(
		uint32 SlotIndex) const -> const FMaterialRenderData&
	{
		static const FMaterialRenderData DefaultMaterial;
		const FMaterialRenderProxyRef& MaterialProxy =
			GetMaterialRenderProxy(SlotIndex);
		return MaterialProxy
			? MaterialProxy->Resolve_RenderThread()
			: DefaultMaterial;
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
	}

	FTextureCubePreviewSceneProxy::FTextureCubePreviewSceneProxy(
		const FStaticMeshRenderData* InRenderData,
		FRHITextureReferenceRef InTextureReference)
		: RenderData(InRenderData)
		, TextureReference(std::move(InTextureReference))
	{
	}
}
