#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

AStaticMeshActor::AStaticMeshActor()
{
	StaticMeshComponent = CreateDefaultComponent<DStaticMeshComponent>("DStaticMeshComponent");
	RootComponent = StaticMeshComponent;
}

AStaticMeshActor::~AStaticMeshActor()
{
}
