#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

AStaticMeshActor::AStaticMeshActor()
{
	DOGE_INFO(STR("AStaticMeshActor::AStaticMeshActor"));
	StaticMeshComponent = NewObject<DStaticMeshComponent>(this, "DStaticMeshComponent");
	RootComponent = StaticMeshComponent;
}
