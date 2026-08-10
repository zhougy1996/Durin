#include "Actors/GameMode.h"

#include "Actors/Pawn.h"
#include "Actors/PlayerController.h"
#include "Actors/PlayerStart.h"
#include "Engine/World.h"

namespace Durin
{
	AGameMode::AGameMode(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto AGameMode::GetPlayerControllerClass() const -> DClass*
	{
		return APlayerController::StaticClass();
	}

	auto AGameMode::GetDefaultPawnClass() const -> DClass*
	{
		return APawn::StaticClass();
	}

	auto AGameMode::ChoosePlayerStart(DWorld& World) const -> APlayerStart*
	{
		for (const TObjectPtr<AActor>& Actor : World.GetActors())
		{
			if (auto* Start = Cast<APlayerStart>(Actor.Get()); Start
				&& !Start->IsPendingKill()
				&& !Start->IsBeingDestroyed())
			{
				return Start;
			}
		}
		return nullptr;
	}
}
