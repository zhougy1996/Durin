#include "Engine/PrimitiveSceneProxy.h"

namespace Doge
{
	auto PrimitiveSceneProxy::SetTransform(FRHICommandList& RHICmdList, const FMatrix& InLocalToWorld, FVector3 InActorPosition) -> void
	{
		LocalToWorld_ = InLocalToWorld;
		ActorPosition_ = InActorPosition;
	}
}