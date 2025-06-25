#pragma once

class FRHICommandList;

class PrimitiveSceneProxy
{
public:
	ENGINE_API auto SetTransform(FRHICommandList& RHICmdList, const FMatrix& InLocalToWorld, FVector InActorPosition) -> void;

private:
	FMatrix LocalToWorld_;

	FVector ActorPosition_;
};