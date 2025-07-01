#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

AStaticMeshActor::AStaticMeshActor()
{
	DStaticMeshComponent* StaticMeshComponent = CreateDefaultComponent<DStaticMeshComponent>("DStaticMeshComponent");
	RootComponent_ = StaticMeshComponent;
	StaticMeshComponent_ = StaticMeshComponent;
}

AStaticMeshActor::~AStaticMeshActor()
{
}
