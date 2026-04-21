#pragma once

#include "Engine/API.h"

namespace Doge
{
	class FRHICommandList;

	class PrimitiveSceneProxy
	{
	public:
		ENGINE_API auto SetTransform(FRHICommandList& RHICmdList, const FMatrix& InLocalToWorld, FVector3 InActorPosition) -> void;

	private:
		FMatrix LocalToWorld_;

		FVector3 ActorPosition_;
	};
}