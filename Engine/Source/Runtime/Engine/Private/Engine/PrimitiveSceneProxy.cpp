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

	FStaticMeshSceneProxy::FStaticMeshSceneProxy(FStaticMeshRenderData* InRenderData, std::vector<FMaterialRenderUpdate> InMaterials)
		: RenderData(InRenderData)
	{
		Materials.resize(InMaterials.size());
		MaterialVersions.resize(InMaterials.size());
		LastMaterialDirtyFlags.resize(InMaterials.size(), EMaterialRenderDirtyFlags::None);
		for (const FMaterialRenderUpdate& Update : InMaterials)
		{
			if (Update.SlotIndex >= Materials.size()) continue;
			Materials[Update.SlotIndex] = Update.RenderData;
			MaterialVersions[Update.SlotIndex] = Update.MaterialVersion;
			LastMaterialDirtyFlags[Update.SlotIndex] = Update.DirtyFlags;
			MaterialComponentRevision = std::max(MaterialComponentRevision, Update.ComponentRevision);
		}
	}

	auto FStaticMeshSceneProxy::GetRenderData() const -> FStaticMeshRenderData*
	{
		return RenderData;
	}

	auto FStaticMeshSceneProxy::GetMaterialRenderData(uint32 SlotIndex) const -> const FMaterialRenderData&
	{
		static const FMaterialRenderData DefaultMaterial;
		return SlotIndex < Materials.size() ? Materials[SlotIndex] : DefaultMaterial;
	}

	auto FStaticMeshSceneProxy::UpdateMaterialRenderData(const FMaterialRenderUpdate& Update) -> void
	{
		CheckRenderingThread();
		if (Update.ComponentRevision <= MaterialComponentRevision) return;
		if (Update.SlotIndex >= Materials.size()) return;
		Materials[Update.SlotIndex] = Update.RenderData;
		MaterialComponentRevision = Update.ComponentRevision;
		MaterialVersions[Update.SlotIndex] = Update.MaterialVersion;
		LastMaterialDirtyFlags[Update.SlotIndex] = Update.DirtyFlags;
	}

	FTextureCubePreviewSceneProxy::FTextureCubePreviewSceneProxy(
		FStaticMeshRenderData* InRenderData,
		FRHITextureReferenceRef InTextureReference)
		: RenderData(InRenderData)
		, TextureReference(std::move(InTextureReference))
	{
	}
}
