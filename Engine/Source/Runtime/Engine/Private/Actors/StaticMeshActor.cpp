#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

AStaticMeshActor::AStaticMeshActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DOGE_INFO(STR("AStaticMeshActor::AStaticMeshActor"));
	StaticMeshComponent = NewObject<DStaticMeshComponent>(this, "DStaticMeshComponent");
	RootComponent = StaticMeshComponent;
}

AStaticMeshActor::~AStaticMeshActor()
{
}
