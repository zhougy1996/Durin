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

	FStaticMeshSceneProxy::FStaticMeshSceneProxy(FStaticMeshRenderData* InRenderData)
		: RenderData(InRenderData)
	{
	}

	auto FStaticMeshSceneProxy::GetRenderData() const -> FStaticMeshRenderData*
	{
		return RenderData;
	}
}
