#pragma once

#include "Engine/Actor.h"

#include "StaticMeshActor.gen.h"

class DStaticMeshComponent;

DCLASS()
class AStaticMeshActor : public AActor
{
	GENERATED_BODY()
public:
	ENGINE_API AStaticMeshActor(const FObjectInitializer& ObjectInitializer);

	ENGINE_API virtual ~AStaticMeshActor();

private:

	DStaticMeshComponent* StaticMeshComponent = nullptr;
};