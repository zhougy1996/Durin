#include "Components/ActorComponent.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/Archive.h"
#include "ComponentReregisterContext.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/ComponentInstanceDataCache.h"
#include "Engine/TransactionAnnotations.h"
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

	auto DActorComponent::FindOrCreateTransactionAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation>
	{
		if (IsTemplateObject()) return nullptr;
		auto State = std::make_shared<FComponentTransactionAnnotation>(*this);
		return State->IsDefault() ? nullptr : State;
	}
	auto DActorComponent::PostEditUndo(std::shared_ptr<ITransactionObjectAnnotation> Annotation) -> void
	{
		if (IsTemplateObject()) { static_cast<DObject*>(this)->PostEditUndo(); return; }
		const auto* Owner = GetOwner();
		const auto* Level = Owner ? Cast<DLevel>(Owner->GetOuter()) : nullptr;
		// Membership commands settle before completion. Historical property data
		// must not reactivate an absent component or an unpublished owning Actor.
		const bool bAbsent = Owner && (!Owner->OwnsComponent(this)
			|| (Level && Level->GetWorld() && !Level->ContainsActor(Owner)));
		if (Annotation)
		{
			auto State = std::dynamic_pointer_cast<FComponentTransactionAnnotation>(Annotation);
			if (!State || State->GetComponent() != this) throw std::runtime_error("Component transaction annotation is incompatible.");
			if (!bAbsent) if (auto Result = State->Restore(); !Result) throw std::runtime_error(Result.error().Message);
		}
		else if (!bAbsent) if (auto Result = FComponentInstanceLifecycleState::DefaultFor(*this).Restore(); !Result)
			throw std::runtime_error(Result.error().Message);
		static_cast<DObject*>(this)->PostEditUndo();
	}
	auto DActorComponent::CaptureLifecycleState() const -> FActorComponentLifecycleState
	{
		if (IsBeingDestroyed() || IsBeginningPlay() || IsEndingPlay()) throw std::runtime_error("Component lifecycle capture requires settled state.");
		return {PrimaryComponentTick.CaptureState(), bool(bHasBeenCreated), bool(bRegistered), bool(bHasBeenInitialized), HasBegunPlay()};
	}
	auto DActorComponent::ValidateLifecycleState(const FActorComponentLifecycleState& State) const -> std::expected<void, std::string>
	{
		if (!IsValid(this) || IsBeingDestroyed() || IsBeginningPlay() || IsEndingPlay()) return std::unexpected("Component lifecycle is unavailable or unsettled.");
		if ((State.bRegistered || State.bInitialized || State.bPlaying) && !State.bCreated) return std::unexpected("Component lifecycle requires creation before activation.");
		if (State.bRegistered && GetOwner() && !GetOwner()->OwnsComponent(this)) return std::unexpected("Registered component must belong to its owner's live set.");
		if (State.bPlaying && (!GetOwner() || !GetOwner()->HasBegunPlay())) return std::unexpected("Component cannot begin play without its playing owner.");
		return FTickFunction::ValidateState(State.Tick, GetOwner() ? Cast<DLevel>(GetOwner()->GetOuter()) : nullptr);
	}
	auto DActorComponent::ApplyLifecycleState(const FActorComponentLifecycleState& State) -> std::expected<void, std::string>
	{
		if (auto Valid = ValidateLifecycleState(State); !Valid) return Valid;
		FGarbageCollectionDeferralScope DeferCollection;
		FScopedPackageDirtySuppression SuppressDirty;
		try
		{
			auto CheckLive = [&] {
				if (!IsValid(this) || IsBeingDestroyed() || (State.bRegistered && GetOwner() && !GetOwner()->OwnsComponent(this)))
					throw std::runtime_error("Component was retired or detached by a lifecycle callback.");
			};
			if (!State.bPlaying && HasBegunPlay()) { RouteEndPlay(); CheckLive(); }
			if (!State.bInitialized && bHasBeenInitialized) { UninitializeComponent(); CheckLive(); }
			if (!State.bRegistered && IsRegistered()) { UnregisterComponent(); CheckLive(); }
			if (!State.bCreated && bHasBeenCreated) { OnComponentDestroyed(); CheckLive(); }
			if (State.bCreated && !bHasBeenCreated) { OnComponentCreated(); CheckLive(); }
			const bool bNeedsRegistration = State.bRegistered || (State.bInitialized && !bHasBeenInitialized)
				|| (State.bPlaying && !HasBegunPlay());
			if (bNeedsRegistration && !IsRegistered()) { RegisterComponentInternal(State.bInitialized); CheckLive(); }
			if (State.bInitialized && !bHasBeenInitialized) { InitializeComponent(); CheckLive(); }
			if (State.bPlaying && !HasBegunPlay()) { DispatchBeginPlay(); CheckLive(); }
			if (!State.bRegistered && IsRegistered()) { UnregisterComponent(); CheckLive(); }
			if (auto Result = PrimaryComponentTick.RestoreState(State.Tick, GetOwner() ? Cast<DLevel>(GetOwner()->GetOuter()) : nullptr); !Result) return Result;
			if (CaptureLifecycleState() != State) return std::unexpected("Component lifecycle restoration did not settle.");
			return {};
		}
		catch (const std::exception& Error) { return std::unexpected(Error.what()); }
		catch (...) { return std::unexpected("Component lifecycle callback threw."); }
	}
	auto DActorComponent::SerializeLifecycleState(FArchive& Archive, FActorComponentLifecycleState& State) -> void
	{
		FTickFunction::SerializeState(Archive, State.Tick);
		Archive << State.bCreated << State.bRegistered << State.bInitialized << State.bPlaying;
	}
	auto DActorComponent::ReregisterComponent() -> void
	{
		FComponentReregisterContext Context(this);
		if (auto Result = Context.ReRegister(); !Result) throw std::runtime_error(Result.error());
	}

	auto DActorComponent::RegisterComponent() -> void { RegisterComponentInternal(true); }

	auto DActorComponent::RegisterComponentInternal(bool bInitialize) -> void
	{
		FGarbageCollectionDeferralScope DeferCollection;
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
		FGarbageCollectionDeferralScope DeferCollection;
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
