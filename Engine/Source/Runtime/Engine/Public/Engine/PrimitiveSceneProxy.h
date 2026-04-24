#pragma once

#include "EngineAPI.h"

namespace Doge
{
	class FRHICommandListBase;

	class PrimitiveSceneProxy
	{
	public:
		ENGINE_API auto SetTransform(FRHICommandListBase& RHICmdList, const FMatrix& InLocalToWorld, FVector3 InActorPosition) -> void;

	private:
		FMatrix LocalToWorld_;

		FVector3 ActorPosition_;
	};
}