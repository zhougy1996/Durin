#include "Engine/Actor.h"

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

auto AActor::RemoveOwnedComponent(DActorComponent* Component) -> void
{
	auto It = std::find(OwnedComponents_.begin(), OwnedComponents_.end(), Component);
	if (It != OwnedComponents_.end())
	{
		OwnedComponents_.erase(It);
	}
}

auto AActor::RemoveInstanceComponent(DActorComponent* Component) -> void
{
	auto It = std::find(InstanceComponents_.begin(), InstanceComponents_.end(), Component);
	if (It != InstanceComponents_.end())
	{
		InstanceComponents_.erase(It);
	}
}

auto AActor::InitializeDefaults() -> void
{
}

