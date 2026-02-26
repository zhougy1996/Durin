#include "Components/ActorComponent.h"

#include "Engine/Actor.h"
#include "Components/SceneComponent.h"

namespace Doge
{
	DActorComponent::DActorComponent(const FObjectInitializer& ObjectInitializer)
		: DObject(ObjectInitializer)
	{
		// TODO: set OwnerActorPrivate
		OwnerActorPrivate = nullptr;
	}
	auto DActorComponent::RegisterComponent() -> void
	{
		ExecuteRegisterEvents();
	}

	auto DActorComponent::UnregisterComponent() -> void
	{
		OnUnregister();
	}

	auto DActorComponent::DestroyComponent() -> void
	{
		if (AActor* Owner = GetOwner())
		{
			Owner->RemoveInstanceComponent(this);
			Owner->RemoveOwnedComponent(this);

			if (Owner->GetRootComponent() == this)
			{
				Owner->SetRootComponent(nullptr);
			}
		}

		OnComponentDestroyed();
		check(!bHasBeenCreated && "Failed to route OnComponentDestroyed()");
	}

	auto DActorComponent::InitializeComponent() -> void
	{
		check(bRegistered);
		check(bHasBeenCreated);
		check(!bHasBeenInitialized);
		bHasBeenInitialized = true;
	}

	auto DActorComponent::UninitializeComponent()->void
	{
		check(bHasBeenInitialized);
		bHasBeenInitialized = false;
	}

	auto DActorComponent::OnRegister() -> void
	{
		bRegistered = true;
	}

	auto DActorComponent::OnUnregister() -> void
	{
		check(bRegistered);
		bRegistered = false;
	}

	auto DActorComponent::OnComponentCreated() -> void
	{
		bHasBeenCreated = true;
	}

	auto DActorComponent::OnComponentDestroyed() -> void
	{
		bHasBeenCreated = false;
	}

	auto DActorComponent::ExecuteRegisterEvents() -> void
	{
		if (!bRegistered)
		{
			OnRegister();
			check(bRegistered && "Failed to route OnRegister()");
		}
	}

	auto DActorComponent::ExecuteUnregisterEvents() -> void
	{
		if (bRegistered)
		{
			OnUnregister();
			check(!bRegistered && "Failed to route OnUnregister()");
		}
	}
}
