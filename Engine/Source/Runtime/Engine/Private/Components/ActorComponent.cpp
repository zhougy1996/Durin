#include "Components/ActorComponent.h"
#include "Threading/RunnableThread.h"
#include "CoreGlobals.h"
#include "EditorSupportDelegates.h"

#include "DObject/ObjectLifecycle.h"
#include "ComponentReregisterContext.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/ComponentInstanceDataCache.h"
#include "Components/SceneComponent.h"

namespace Durin
{
	DActorComponent::DActorComponent(const FObjectInitializer& ObjectInitializer)
		: DObject(ObjectInitializer)
	{
		OwnerActorPrivate = Cast<AActor>(ObjectInitializer.Outer);
		PrimaryComponentTick.SetTarget(this);
	}
	auto DActorComponent::PostInitProperties() -> void
	{
		Super::PostInitProperties();
		if (OwnerActorPrivate && CreationMethod != EComponentCreationMethod::Instance)
			OwnerActorPrivate->AddOwnedComponent(this);
	}
	auto DActorComponent::GetComponentInstanceData() const -> std::shared_ptr<FActorComponentInstanceData> { return {}; }

	auto DActorComponent::DeactivateForRemoval() -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!IsValid(this) || IsBeingDestroyed() || IsBeginningPlay() || IsEndingPlay())
			throw std::runtime_error("Cannot remove a component during a lifecycle transition.");
		auto CheckLive = [this] {
			if (!IsValid(this) || IsBeingDestroyed())
				throw std::runtime_error("Component was retired by a removal callback.");
		};
		if (HasBegunPlay()) { RouteEndPlay(); CheckLive(); }
		if (bHasBeenInitialized) { UninitializeComponent(); CheckLive(); }
		UnregisterComponent();
		CheckLive();
		if (bHasBeenCreated) { OnComponentDestroyed(); CheckLive(); }
	}

	auto DActorComponent::ReregisterComponent() -> void
	{
		FComponentReregisterContext Context(this);
		if (auto Result = Context.ReRegister(); !Result) throw std::runtime_error(Result.error());
	}

	auto DActorComponent::RegisterComponent() -> void { RegisterComponentInternal(true); }

	auto DActorComponent::RegisterComponentInternal(bool bInitialize) -> void
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (IsPendingKill())
		{
			DURIN_WARN(
				"Component registration rejected after object teardown began. (component: {})",
				GetObjectPath());
			return;
		}
		if (!bHasBeenCreated) OnComponentCreated();
		if (!IsValid(this) || IsBeingDestroyed()) return;
		ExecuteRegisterEvents();
		if (!IsValid(this) || IsBeingDestroyed()) return;
		if (bInitialize && !bHasBeenInitialized) InitializeComponent();
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
		if (GIsGameThreadIdInitialized) CheckGameThread();
		PrimaryComponentTick.UnregisterTickFunction();
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

		MarkAsGarbage();
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
#if DURIN_WITH_EDITOR
		if (auto* Level = GetOwner() ? Cast<DLevel>(GetOwner()->GetOuter()) : nullptr)
			FEditorSupportDelegates::InvalidateHitProxies.Broadcast(*Level);
#endif
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
		if (auto* Level = GetOwner() ? Cast<DLevel>(GetOwner()->GetOuter()) : nullptr; Level && Level->GetWorld())
			Level->GetWorld()->GetTimerManager().ClearAllTimersForObject(this);
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
			if (!IsValid(this) || IsBeingDestroyed()) return;
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
