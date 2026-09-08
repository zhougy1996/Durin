#include "Engine/World.h"
#include "Engine/Engine.h"
#include "WorldOperation.h"

#include "Actors/Controller.h"
#include "Actors/GameMode.h"
#include "Actors/Pawn.h"
#include "Actors/PlayerController.h"
#include "Components/ActorComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	DWorld::DWorld(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer), Subsystems(*this)
	{
	}

	auto DWorld::SetWorldType(EWorldType InType) -> bool
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (GetSubsystemState() != EWorldSubsystemState::Uninitialized) return false;
		WorldType = InType;
		return true;
	}

	auto DWorld::InitializeSubsystems() -> FWorldSubsystemResult
	{
		if (Operation != EOperation::Idle)
			return {EWorldSubsystemError::InvalidState, "World operation is in progress."};
		TStrongObjectPtr<DWorld> InitializationGuard(this);
		std::optional<DEngine::FWorldInitializationScope> HostScope;
		if (auto* Host = Cast<DEngine>(GetOuter())) HostScope.emplace(*Host, *this);
		FOperationScope OperationScope(*this, EOperation::Initializing);
		auto Result = Subsystems.Initialize();
		return Result;
	}

	auto DWorld::AddReferencedObjects(FReferenceCollector& Collector) -> void
	{
		Super::AddReferencedObjects(Collector);
		Subsystems.AddReferencedObjects(Collector);
		for (auto* Transition : {&PendingLevelTransition, &ActiveLevelTransition})
		{
			if (!*Transition) continue;
			DObject* Level = (*Transition)->Level.Get();
			Collector.AddReferencedObject(Level);
		}
	}

	auto DWorld::CanDispatchSubsystems() const -> bool
	{
		return !IsPendingKill() && GetSubsystemState() == EWorldSubsystemState::Ready && !bShutdownRequested
			&& !bEndPlayRequested && !PendingLevelTransition;
	}

	auto DWorld::FlushLifecycleRequests() -> void
	{
		if (Operation != EOperation::Idle) return;
		if (bShutdownRequested) Shutdown();
		else if (std::exchange(bEndPlayRequested, false)) EndPlay();
	}

	auto DWorld::RequestShutdown() -> void
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		bShutdownRequested = true;
		Subsystems.CloseWork();
	}

	auto DWorld::Shutdown() -> void
	{
		RequestShutdown();
		if (Operation != EOperation::Idle) return;
		if (GetSubsystemState() == EWorldSubsystemState::ShuttingDown || GetSubsystemState() == EWorldSubsystemState::Shutdown) return;
		FOperationScope OperationScope(*this, EOperation::ShuttingDown);
		EndPlayInternal();
		PendingLevelTransition.reset();
		SetCurrentLevelInternal(nullptr, true);
		Subsystems.Shutdown();
		RenderScene = nullptr;
	}

	auto DWorld::IsReadyForFinishDestroy() -> bool
	{
		return Operation == EOperation::Idle;
	}

	auto DWorld::BeginDestroy() -> void
	{
		Shutdown();
		Super::BeginDestroy();
	}

	auto DWorld::SpawnActor(DClass* ActorClass, FName InName) -> AActor*
	{
		return CurrentLevel ? CurrentLevel->SpawnActor(ActorClass, InName) : nullptr;
	}

	auto DWorld::DestroyActor(AActor* Actor) -> bool
	{
		return CurrentLevel && CurrentLevel->DestroyActor(Actor);
	}

	auto DWorld::DestroyAllActors() -> void
	{
		if (CurrentLevel) CurrentLevel->DestroyAllActors();
	}

	auto DWorld::ContainsActor(const AActor* Actor) const -> bool
	{
		return CurrentLevel && CurrentLevel->ContainsActor(Actor);
	}

	auto DWorld::FindActorByName(FName Name) const -> AActor*
	{
		return CurrentLevel ? CurrentLevel->FindActorByName(Name) : nullptr;
	}

	auto DWorld::GetActors() const -> const std::vector<TObjectPtr<AActor>>&
	{
		static const std::vector<TObjectPtr<AActor>> Empty;
		return CurrentLevel ? CurrentLevel->GetActors() : Empty;
	}

	auto DWorld::SetCurrentLevel(DLevel* Level, bool bDestroyPreviousOwnedLevel) -> bool
	{
		if (Operation != EOperation::Idle) return false;
		FOperationScope OperationScope(*this, EOperation::ChangingLevel);
		return SetCurrentLevelInternal(Level, bDestroyPreviousOwnedLevel);
	}

	auto DWorld::SetCurrentLevelInternal(DLevel* Level, bool bDestroyPreviousOwnedLevel) -> bool
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (Level && GetSubsystemState() != EWorldSubsystemState::Ready) return false;
		if (Level && bShutdownRequested) return false;
		if (Level == CurrentLevel.Get()) return true;
		if (PlayState != EWorldPlayState::Stopped) return false;
		if (Level && Level->IsPendingKill()) return false;
		if (Level && Level->GetWorld() && Level->GetWorld() != this) return false;
		if (Level && Cast<DWorld>(Level->GetOuter()) && Level->GetOuter() != this) return false;
		TStrongObjectPtr<DLevel> AttachmentGuard(Level);
		DLevel* Previous = CurrentLevel.Get();
		TStrongObjectPtr<DLevel> PreviousGuard(Previous);
		if (Previous)
		{
			for (const TObjectPtr<AActor>& Actor : Previous->GetActors())
			{
				if (auto* Controller = Cast<AController>(Actor.Get())) Controller->UnPossess();
			}
			std::vector<TObjectPtr<DActorComponent>> Components;
			for (const TObjectPtr<AActor>& Actor : Previous->GetActors())
			{
				if (!Actor) continue;
				for (const TObjectPtr<DActorComponent>& Component : Actor->GetComponents())
				{
					if (Component && Component->IsRegistered()) Components.push_back(Component);
				}
			}
			for (const TObjectPtr<DActorComponent>& Component : Components)
			{
				if (Component && Component->IsRegistered()) Component->UnregisterComponent();
			}
			Subsystems.LevelChanged(*Previous, false);
			Previous->TickRegistry.Reset();
			Previous->SetOwningWorld(nullptr);
		}
		CurrentLevel = Level;
		if (Level)
		{
			Level->SetOwningWorld(this);
			Subsystems.LevelChanged(*Level, true);
			const std::vector<TObjectPtr<AActor>> Actors = Level->GetActors();
			for (const TObjectPtr<AActor>& Actor : Actors)
			{
				if (CurrentLevel.Get() != Level || !CanDispatchSubsystems()) break;
				if (Actor && !Actor->IsPendingKill() && Actor->GetOuter() == Level
					&& !Actor->IsBeingDestroyed()) Level->OnActorAdded(Actor.Get());
			}
		}
		if (bDestroyPreviousOwnedLevel && Previous && Previous->GetOuter() == this)
			MarkObjectHierarchyAsGarbage(Previous);
		return !bShutdownRequested;
	}

	auto DWorld::RequestLevelTransition(DLevel* Level, bool bDestroyPreviousOwnedLevel) -> bool
	{
		require(!GIsGameThreadIdInitialized || IsInGameThread());
		if (bShutdownRequested || GetSubsystemState() != EWorldSubsystemState::Ready) return false;
		if (Operation == EOperation::ChangingLevel && Level == CurrentLevel.Get()
			&& (!ActiveLevelTransition || ActiveLevelTransition->Level.Get() == Level)) return true;
		if (Level && Level->IsPendingKill()) return false;
		if (Level && Level->GetWorld() && Level->GetWorld() != this) return false;
		if (Level && Cast<DWorld>(Level->GetOuter()) && Level->GetOuter() != this) return false;
		PendingLevelTransition = FPendingLevelTransition{
			.Level = Level,
			.GameModeClass = GameplaySession && GameplaySession->GameMode
				? GameplaySession->GameMode->GetClass() : nullptr,
			.bDestroyPreviousOwnedLevel = bDestroyPreviousOwnedLevel,
			.bResumePlay = PlayState == EWorldPlayState::BeginningPlay || PlayState == EWorldPlayState::Playing};
		return true;
	}

	auto DWorld::SetRenderScene(FSceneInterface* InRenderScene) -> void
	{
		if (RenderScene == InRenderScene) return;
		if (Operation != EOperation::Idle || bShutdownRequested) return;
		FOperationScope OperationScope(*this, EOperation::ChangingScene);
		std::vector<TObjectPtr<DActorComponent>> RegisteredComponents;
		if (CurrentLevel)
		{
			for (const TObjectPtr<AActor>& Actor : CurrentLevel->GetActors())
			{
				if (!Actor) continue;
				for (const TObjectPtr<DActorComponent>& Component : Actor->GetComponents())
					if (Component && Component->IsRegistered()) RegisteredComponents.push_back(Component);
			}
		}
		for (const TObjectPtr<DActorComponent>& Component : RegisteredComponents)
			if (Component && Component->IsRegistered()) Component->UnregisterComponent();
		RenderScene = InRenderScene;
		for (const TObjectPtr<DActorComponent>& Component : RegisteredComponents)
			if (Component && !Component->IsPendingKill()) Component->RegisterComponent();
	}

	auto DWorld::OnActorDestroyed(AActor* Actor) -> void
	{
		if (!Actor) return;
		if (CurrentLevel)
		{
			const std::vector<TObjectPtr<AActor>> Actors = CurrentLevel->GetActors();
			for (const TObjectPtr<AActor>& Candidate : Actors)
				if (auto* PlayerController = Cast<APlayerController>(Candidate.Get()))
					PlayerController->HandleViewTargetDestroyed(Actor);
		}
		if (!GameplaySession) return;
		if (GameplaySession->DefaultPawn.Get() == Actor) GameplaySession->DefaultPawn = nullptr;
		if (GameplaySession->LocalPlayerController.Get() == Actor) GameplaySession->LocalPlayerController = nullptr;
		if (GameplaySession->GameMode.Get() == Actor) GameplaySession->GameMode = nullptr;
		std::erase_if(GameplaySession->RuntimeActors,
			[Actor](const TObjectPtr<AActor>& RuntimeActor) { return RuntimeActor.Get() == Actor; });
	}

	auto DWorld::ClearPendingGameplayIntent() -> void
	{
		if (GameplaySession && GameplaySession->DefaultPawn)
			GameplaySession->DefaultPawn->ClearPendingControlIntent();
	}
}
