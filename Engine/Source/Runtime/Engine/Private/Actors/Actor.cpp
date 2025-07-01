#include "Actors/Actor.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"

AActor::AActor()
{
	InitializeDefaults();
}

AActor::~AActor()
{
	InstanceComponents_.clear();
	RootComponent_ = nullptr;

	for (auto* Component : OwnedComponents_)
	{
		delete Component;
	}

	OwnedComponents_.clear();
}

auto AActor::InitializeDefaults() -> void
{
}

