#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

AStaticMeshActor::AStaticMeshActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	StaticMeshComponent = NewObject<DStaticMeshComponent>(this, "DStaticMeshComponent");
	RootComponent = StaticMeshComponent;
}

AStaticMeshActor::~AStaticMeshActor()
{
}
