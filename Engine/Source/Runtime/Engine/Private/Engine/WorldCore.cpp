#include "Engine/World.h"

#include "Actors/Controller.h"
#include "Actors/GameMode.h"
#include "Actors/Pawn.h"
#include "Actors/PlayerController.h"
#include "Components/ActorComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"

namespace Durin
{
	DWorld::DWorld(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DWorld::BeginDestroy() -> void
	{
		EndPlay();
		SetCurrentLevel(nullptr);
		RenderScene = nullptr;
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
		if (Level == CurrentLevel.Get()) return true;
		if (PlayState != EWorldPlayState::Stopped) return false;
		if (Level && Level->GetWorld() && Level->GetWorld() != this) return false;
		if (Level && Cast<DWorld>(Level->GetOuter()) && Level->GetOuter() != this) return false;
		EndPlay();
		DLevel* Previous = CurrentLevel.Get();
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
			Previous->TickRegistry.Reset();
			Previous->SetOwningWorld(nullptr);
		}
		CurrentLevel = Level;
		if (Level)
		{
			Level->SetOwningWorld(this);
			const std::vector<TObjectPtr<AActor>> Actors = Level->GetActors();
			for (const TObjectPtr<AActor>& Actor : Actors)
			{
				if (CurrentLevel.Get() != Level) break;
				if (Actor && !Actor->IsPendingKill() && Actor->GetOuter() == Level
					&& !Actor->IsBeingDestroyed()) Level->OnActorAdded(Actor.Get());
			}
		}
		if (bDestroyPreviousOwnedLevel && Previous && Previous->GetOuter() == this)
			MarkObjectHierarchyAsGarbage(Previous);
		return true;
	}

	auto DWorld::RequestLevelTransition(DLevel* Level, bool bDestroyPreviousOwnedLevel) -> bool
	{
		if (Level && Level->GetWorld() && Level->GetWorld() != this) return false;
		if (Level && Cast<DWorld>(Level->GetOuter()) && Level->GetOuter() != this) return false;
		PendingLevelTransition = FPendingLevelTransition{
			.Level = Level,
			.GameModeClass = GameplaySession && GameplaySession->GameMode
				? GameplaySession->GameMode->GetClass() : nullptr,
			.bDestroyPreviousOwnedLevel = bDestroyPreviousOwnedLevel,
			.bResumePlay = HasBegunPlay()};
		return true;
	}

	auto DWorld::SetRenderScene(IScene* InRenderScene) -> void
	{
		if (RenderScene == InRenderScene) return;
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
