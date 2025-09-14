#pragma once

#include "Components/ActorComponent.h"

#include "SceneComponent.gen.h"

DCLASS()
class DSceneComponent : public DActorComponent
{
	GENERATED_BODY()
public:

protected:
	FTransform ComponentToWorld_;
};