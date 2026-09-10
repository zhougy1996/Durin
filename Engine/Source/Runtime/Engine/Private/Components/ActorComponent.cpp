#include "Components/ActorComponent.h"

#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Components/SceneComponent.h"

namespace Durin
{
	DActorComponent::DActorComponent(const FObjectInitializer& ObjectInitializer)
		: DObject(ObjectInitializer)
	{
		OwnerActorPrivate = Cast<AActor>(ObjectInitializer.Outer);
		PrimaryComponentTick.SetTarget(this);
	}
	auto DActorComponent::RegisterComponent() -> void
	{
		if (IsPendingKill())
		{
			DURIN_WARN(
				"Component registration rejected after object teardown began. (component: {})",
				GetObjectPath());
			return;
		}
		if (!bHasBeenCreated) OnComponentCreated();
		ExecuteRegisterEvents();
		if (!bHasBeenInitialized) InitializeComponent();
		if (bRegistered && !IsBeingDestroyed())
		{
			if (DLevel* Level = OwnerActorPrivate ? Cast<DLevel>(OwnerActorPrivate->GetOuter()) : nullptr;
				Level && Level->GetWorld())
			{
				PrimaryComponentTick.RegisterTickFunction(Level);
			}
		}
	}

	auto DActorComponent::UnregisterComponent() -> void
	{
		PrimaryComponentTick.UnregisterTickFunction();
		RouteEndPlay();
		if (bHasBeenInitialized) UninitializeComponent();
		ExecuteUnregisterEvents();
	}

	auto DActorComponent::DestroyComponent() -> void
	{
		if (DestructionState == EComponentDestructionState::Destroying) return;
		PrimaryComponentTick.CancelPendingTick();
		if (PlayState == EComponentPlayState::BeginningPlay
			|| PlayState == EComponentPlayState::EndingPlay)
		{
			DestructionState = EComponentDestructionState::Requested;
			return;
		}

		DestructionState = EComponentDestructionState::Destroying;
		PrimaryComponentTick.UnregisterTickFunction();
		OnComponentPendingKill();

		if (AActor* Owner = GetOwner())
		{
			Owner->RemoveOwnedComponent(this);

			if (Owner->GetRootComponent() == this)
			{
				Owner->SetRootComponent(nullptr);
			}
		}

		MarkAsGarbage(this);
	}

	auto DActorComponent::BeginDestroy() -> void
	{
		PrimaryComponentTick.UnregisterTickFunction();
		DestructionState = EComponentDestructionState::Destroying;
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
		RouteEndPlay();
		if (bHasBeenInitialized) UninitializeComponent();
		ExecuteUnregisterEvents();
		if (bHasBeenCreated)
		{
			OnComponentDestroyed();
			check(!bHasBeenCreated && "Failed to route OnComponentDestroyed()");
		}
	}

	auto DActorComponent::OnOwnerVisibilityChanged() -> void
	{
	}

	auto DActorComponent::DispatchBeginPlay() -> void
	{
		if (PlayState != EComponentPlayState::NotBegun
			|| DestructionState != EComponentDestructionState::Alive
			|| IsPendingKill()
			|| !bRegistered)
		{
			return;
		}

		PlayState = EComponentPlayState::BeginningPlay;
		BeginPlay();
		if (PlayState == EComponentPlayState::BeginningPlay)
		{
			PlayState = EComponentPlayState::Playing;
			PrimaryComponentTick.NotifyEligibilityChanged();
		}

		if (bEndPlayRequested)
		{
			bEndPlayRequested = false;
			RouteEndPlay();
		}

		if (DestructionState == EComponentDestructionState::Requested) DestroyComponent();
	}

	auto DActorComponent::RouteEndPlay() -> void
	{
		if (PlayState == EComponentPlayState::NotBegun || PlayState == EComponentPlayState::EndingPlay) return;
		if (PlayState == EComponentPlayState::BeginningPlay)
		{
			bEndPlayRequested = true;
			return;
		}

		PrimaryComponentTick.CancelPendingTick();
		PlayState = EComponentPlayState::EndingPlay;
		EndPlay();
		if (PlayState == EComponentPlayState::EndingPlay) PlayState = EComponentPlayState::NotBegun;

		if (DestructionState == EComponentDestructionState::Requested) DestroyComponent();
	}

	auto DActorComponent::BeginPlay() -> void
	{
		check(bRegistered);
	}

	auto DActorComponent::TickComponent(float DeltaSeconds) -> void
	{
		(void)DeltaSeconds;
	}

	auto DActorComponent::EndPlay() -> void
	{
	}

	auto DActorComponent::ExecuteRegisterEvents() -> void
	{
		if (!bRegistered)
		{
			OnRegister();
			check(bRegistered && "Failed to route OnRegister()");
			++RegistrationGeneration;
		}
	}

	auto DActorComponent::ExecuteUnregisterEvents() -> void
	{
		if (bRegistered)
		{
			OnUnregister();
			check(!bRegistered && "Failed to route OnUnregister()");
			++RegistrationGeneration;
		}
	}
}
