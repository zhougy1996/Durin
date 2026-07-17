#include "Components/ActorComponent.h"

#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Components/SceneComponent.h"

namespace Durin
{
	DActorComponent::DActorComponent(const FObjectInitializer& ObjectInitializer)
		: DObject(ObjectInitializer)
	{
		OwnerActorPrivate = dynamic_cast<AActor*>(ObjectInitializer.Outer);
	}
	auto DActorComponent::RegisterComponent() -> void
	{
		if (!bHasBeenCreated) OnComponentCreated();
		ExecuteRegisterEvents();
		if (!bHasBeenInitialized) InitializeComponent();
	}

	auto DActorComponent::UnregisterComponent() -> void
	{
		if (bHasBegunPlay) EndPlay();
		if (bHasBeenInitialized) UninitializeComponent();
		ExecuteUnregisterEvents();
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

		OnComponentPendingKill();
		MarkAsGarbage(this);
	}

	auto DActorComponent::BeginDestroy() -> void
	{
		OnComponentPendingKill();
		Super::BeginDestroy();
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

	auto DActorComponent::OnComponentPendingKill() -> void
	{
		if (bHasBegunPlay) EndPlay();
		if (bHasBeenInitialized) UninitializeComponent();
		ExecuteUnregisterEvents();
		if (bHasBeenCreated)
		{
			OnComponentDestroyed();
			check(!bHasBeenCreated && "Failed to route OnComponentDestroyed()");
		}
	}

	auto DActorComponent::BeginPlay() -> void
	{
		check(bRegistered);
		check(!bHasBegunPlay);
		bHasBegunPlay = true;
	}

	auto DActorComponent::TickComponent(float DeltaSeconds) -> void
	{
		(void)DeltaSeconds;
	}

	auto DActorComponent::EndPlay() -> void
	{
		check(bHasBegunPlay);
		bHasBegunPlay = false;
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
