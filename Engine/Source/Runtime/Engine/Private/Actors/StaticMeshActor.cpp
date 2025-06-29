#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

AStaticMeshActor::AStaticMeshActor()
{
	TSharedPtr<DStaticMeshComponent> StaticMeshComponent = std::make_shared<DStaticMeshComponent>(this);
	OwnedComponents_.push_back(StaticMeshComponent);
	RootComponent_ = StaticMeshComponent.get();
	StaticMeshComponent_ = StaticMeshComponent.get();
}

AStaticMeshActor::~AStaticMeshActor()
{
}
