#pragma once

#include "Components/ActorComponent.h"

class DSceneComponent : public DActorComponent
{
public:

protected:
	FTransform ComponentToWorld_;
};