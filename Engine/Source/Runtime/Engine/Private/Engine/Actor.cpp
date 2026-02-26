#include "Engine/Actor.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"

namespace Doge
{
	AActor::AActor()
	{
		InitializeDefaults();
	}

	AActor::AActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		InitializeDefaults();
	}

	AActor::~AActor()
	{
		InstanceComponents.clear();
		RootComponent = nullptr;

		for (auto* Component : OwnedComponents)
		{
			delete Component;
		}

		OwnedComponents.clear();
	}

	auto AActor::RemoveOwnedComponent(DActorComponent* Component) -> void
	{
		auto It = std::find(OwnedComponents.begin(), OwnedComponents.end(), Component);
		if (It != OwnedComponents.end())
		{
			OwnedComponents.erase(It);
		}
	}

	auto AActor::RemoveInstanceComponent(DActorComponent* Component) -> void
	{
		auto It = std::find(InstanceComponents.begin(), InstanceComponents.end(), Component);
		if (It != InstanceComponents.end())
		{
			InstanceComponents.erase(It);
		}
	}

	auto AActor::InitializeDefaults() -> void
	{
	}
}
