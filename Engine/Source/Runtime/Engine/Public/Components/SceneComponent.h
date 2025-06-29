#pragma once

#include "Components/ActorComponent.h"

class DSceneComponent : public DActorComponent
{
public:
	ENGINE_API DSceneComponent(AActor* OwnerActor);

protected:
	FTransform ComponentToWorld_;
};