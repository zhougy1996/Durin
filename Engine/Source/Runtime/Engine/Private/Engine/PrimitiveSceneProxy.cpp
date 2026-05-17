#include "Engine/PrimitiveSceneProxy.h"

namespace Durin
{
	auto PrimitiveSceneProxy::SetTransform(FRHICommandListBase& RHICmdList, const FMatrix& InLocalToWorld, FVector3 InActorPosition) -> void
	{
		LocalToWorld_ = InLocalToWorld;
		ActorPosition_ = InActorPosition;
	}
}