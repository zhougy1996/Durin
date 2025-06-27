#pragma once

#include "Components/ActorComponent.h"

class DSceneComponent : public DActorComponent
{
public:
	ENGINE_API DSceneComponent();

protected:
	FTransform ComponentToWorld_;
};