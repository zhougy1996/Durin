#include "Engine/PrimitiveSceneProxy.h"

auto PrimitiveSceneProxy::SetTransform(FRHICommandList& RHICmdList, const FMatrix& InLocalToWorld, FVector InActorPosition) -> void
{
	LocalToWorld_ = InLocalToWorld;
	ActorPosition_ = InActorPosition;
}
