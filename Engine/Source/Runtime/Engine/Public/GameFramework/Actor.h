#pragma once

class DActorComponent;
class DSceneComponent;

class AActor
{
public:
	ENGINE_API AActor();

protected:
	DSceneComponent* RootComponent_ = nullptr;

	TArray<DActorComponent*> OwnedComponents_;

	TArray<DActorComponent*> InstanceComponents_;
};