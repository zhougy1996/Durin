#pragma once

#include "Actors/Actor.h"

class DStaticMeshComponent;

class AStaticMeshActor : public AActor
{
public:
	ENGINE_API AStaticMeshActor();

	ENGINE_API virtual ~AStaticMeshActor();

private:

	DStaticMeshComponent* StaticMeshComponent_ = nullptr;
};