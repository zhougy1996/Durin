#include "Components/ActorComponent.h"

#include "Actors/Actor.h"

DActorComponent::DActorComponent(AActor* OwnerActor)
	: OwnerActor_(OwnerActor)
{
}

auto DActorComponent::DestroyComponent() -> void
{
	AActor* Owner = GetOwner();
}

