#include "Engine/PrimitiveSceneProxy.h"

namespace Doge
{
	auto PrimitiveSceneProxy::SetTransform(FRHICommandListBase& RHICmdList, const FMatrix& InLocalToWorld, FVector3 InActorPosition) -> void
	{
		LocalToWorld_ = InLocalToWorld;
		ActorPosition_ = InActorPosition;
	}
}