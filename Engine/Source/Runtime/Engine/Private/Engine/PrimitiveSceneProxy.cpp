#include "Engine/PrimitiveSceneProxy.h"

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

	FStaticMeshSceneProxy::FStaticMeshSceneProxy(FStaticMeshRenderData* InRenderData, std::vector<FMaterialRenderData> InMaterials)
		: RenderData(InRenderData)
		, Materials(std::move(InMaterials))
	{
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
}
