#pragma once

#include "Actors/Actor.h"

class DStaticMeshComponent;

class ENGINE_API AStaticMeshActor : public AActor
{
public:
	AStaticMeshActor();

	virtual ~AStaticMeshActor();

private:

	DStaticMeshComponent* StaticMeshComponent_ = nullptr;
};