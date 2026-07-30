#include "Engine/World.h"

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
		if (Level && Level->GetWorld() && Level->GetWorld() != this) return false;
		if (Level && Cast<DWorld>(Level->GetOuter()) && Level->GetOuter() != this) return false;
		EndPlay();
		DLevel* Previous = CurrentLevel.Get();
		if (Previous)
		{
			std::vector<TObjectPtr<DActorComponent>> Components;
			for (const TObjectPtr<AActor>& Actor : Previous->GetActors())
			{
				if (!Actor) continue;
				for (const TObjectPtr<DActorComponent>& Component : Actor->GetOwnedComponents())
				{
					if (Component && Component->IsRegistered()) Components.push_back(Component);
				}
			}
			for (const TObjectPtr<DActorComponent>& Component : Components)
			{
				if (Component && !Component->IsPendingKill() && Component->IsRegistered()) Component->UnregisterComponent();
			}
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
				if (Actor
					&& !Actor->IsPendingKill()
					&& Actor->GetOuter() == Level
					&& !Actor->IsBeingDestroyed())
				{
					Level->OnActorAdded(Actor.Get());
				}
			}
		}
		if (bDestroyPreviousOwnedLevel && Previous && Previous->GetOuter() == this) MarkObjectHierarchyAsGarbage(Previous);
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
				for (const TObjectPtr<DActorComponent>& Component : Actor->GetOwnedComponents())
				{
					if (Component && Component->IsRegistered()) RegisteredComponents.push_back(Component);
				}
			}
		}
		for (const TObjectPtr<DActorComponent>& Component : RegisteredComponents)
		{
			if (Component && !Component->IsPendingKill() && Component->IsRegistered()) Component->UnregisterComponent();
		}

		RenderScene = InRenderScene;
		for (const TObjectPtr<DActorComponent>& Component : RegisteredComponents)
		{
			if (Component && !Component->IsPendingKill()) Component->RegisterComponent();
		}
	}

	auto DWorld::BeginPlay() -> void
	{
		if (PlayState != EWorldPlayState::Stopped || !CurrentLevel) return;
		DLevel* CapturedLevel = CurrentLevel.Get();
		const std::vector<TObjectPtr<AActor>> Actors = CapturedLevel->GetActors();
		PlayState = EWorldPlayState::BeginningPlay;
		for (const TObjectPtr<AActor>& Actor : Actors)
		{
			if (PlayState != EWorldPlayState::BeginningPlay || CurrentLevel.Get() != CapturedLevel) break;
			if (Actor
				&& !Actor->IsPendingKill()
				&& Actor->GetOuter() == CapturedLevel
				&& !Actor->IsBeingDestroyed()
				&& !Actor->HasBegunPlay())
			{
				Actor->DispatchBeginPlay();
			}
		}
		if (PlayState == EWorldPlayState::BeginningPlay) PlayState = EWorldPlayState::Playing;
	}

	auto DWorld::Tick(float DeltaSeconds) -> void
	{
		if (!HasBegunPlay() || !CurrentLevel) return;
		if (bPaused && !std::exchange(bSingleStepRequested, false)) return;
		const std::vector<TObjectPtr<AActor>> Actors = CurrentLevel->GetActors();
		for (const TObjectPtr<AActor>& Actor : Actors)
		{
			if (Actor && Actor->HasBegunPlay() && Actor->IsActorTickEnabled()) Actor->Tick(DeltaSeconds);
		}
	}

	auto DWorld::EndPlay() -> void
	{
		if (PlayState == EWorldPlayState::Stopped || PlayState == EWorldPlayState::EndingPlay) return;
		DLevel* CapturedLevel = CurrentLevel.Get();
		std::vector<TObjectPtr<AActor>> Actors;
		if (CapturedLevel) Actors = CapturedLevel->GetActors();
		PlayState = EWorldPlayState::EndingPlay;
		bSingleStepRequested = false;
		for (auto It = Actors.rbegin(); It != Actors.rend(); ++It)
		{
			if (PlayState != EWorldPlayState::EndingPlay || CurrentLevel.Get() != CapturedLevel) break;
			if (*It
				&& !(*It)->IsPendingKill()
				&& (*It)->GetOuter() == CapturedLevel
				&& !(*It)->IsBeingDestroyed()
				&& (*It)->HasBegunPlay())
			{
				(*It)->RouteEndPlay();
			}
		}
		if (PlayState == EWorldPlayState::EndingPlay) PlayState = EWorldPlayState::Stopped;
	}
} // namespace Durin
