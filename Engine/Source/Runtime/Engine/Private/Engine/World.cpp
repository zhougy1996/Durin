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
		if (Level && dynamic_cast<DWorld*>(Level->GetOuter()) && Level->GetOuter() != this) return false;
		EndPlay();
		DLevel* Previous = CurrentLevel.Get();
		if (Previous)
		{
			for (const TObjectPtr<AActor>& Actor : Previous->GetActors())
			{
				for (const TObjectPtr<DActorComponent>& Component : Actor->GetOwnedComponents())
				{
					if (Component && Component->IsRegistered()) Component->UnregisterComponent();
				}
			}
			Previous->SetOwningWorld(nullptr);
		}
		CurrentLevel = Level;
		if (Level)
		{
			Level->SetOwningWorld(this);
			for (const TObjectPtr<AActor>& Actor : Level->GetActors()) Level->OnActorAdded(Actor.Get());
		}
		if (bDestroyPreviousOwnedLevel && Previous && Previous->GetOuter() == this) MarkObjectHierarchyAsGarbage(Previous);
		return true;
	}

	auto DWorld::BeginPlay() -> void
	{
		if (bHasBegunPlay || !CurrentLevel) return;
		bHasBegunPlay = true;
		for (const TObjectPtr<AActor>& Actor : CurrentLevel->GetActors())
		{
			if (Actor && !Actor->HasBegunPlay()) Actor->BeginPlay();
		}
	}

	auto DWorld::Tick(float DeltaSeconds) -> void
	{
		if (!bHasBegunPlay || !CurrentLevel) return;
		if (bPaused && !std::exchange(bSingleStepRequested, false)) return;
		const std::vector<TObjectPtr<AActor>> Actors = CurrentLevel->GetActors();
		for (const TObjectPtr<AActor>& Actor : Actors)
		{
			if (Actor && Actor->HasBegunPlay() && Actor->IsActorTickEnabled()) Actor->Tick(DeltaSeconds);
		}
	}

	auto DWorld::EndPlay() -> void
	{
		if (!bHasBegunPlay) return;
		if (CurrentLevel)
		{
			for (auto It = CurrentLevel->GetActors().rbegin(); It != CurrentLevel->GetActors().rend(); ++It)
			{
				if (*It && (*It)->HasBegunPlay()) (*It)->EndPlay();
			}
		}
		bHasBegunPlay = false;
		bSingleStepRequested = false;
	}
} // namespace Durin
