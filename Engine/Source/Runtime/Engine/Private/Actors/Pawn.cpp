#include "Actors/Pawn.h"

#include "Actors/Controller.h"
#include "Components/PawnMovementComponent.h"
#include "Components/SceneComponent.h"

namespace Durin
{
	APawn::APawn(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SetRootComponent(CreateDefaultComponent<DSceneComponent>("Root"));
		SetActorTickEnabled(true);
	}

	auto APawn::AdmitControlIntent(const FPawnControlIntent& Intent) -> bool
	{
		if (bHasPendingControlIntent
			|| !std::isfinite(Intent.Move.x)
			|| !std::isfinite(Intent.Move.y)
			|| !std::isfinite(Intent.Look.x)
			|| !std::isfinite(Intent.Look.y))
		{
			return false;
		}
		PendingControlIntent = Intent;
		PendingControlIntent.Move.x = std::clamp(PendingControlIntent.Move.x, -1.0, 1.0);
		PendingControlIntent.Move.y = std::clamp(PendingControlIntent.Move.y, -1.0, 1.0);
		PendingControlIntent.Look.x = std::clamp(PendingControlIntent.Look.x, -1.0, 1.0);
		PendingControlIntent.Look.y = std::clamp(PendingControlIntent.Look.y, -1.0, 1.0);
		bHasPendingControlIntent = true;
		return true;
	}

	auto APawn::ClearPendingControlIntent() -> void
	{
		PendingControlIntent = {};
		bHasPendingControlIntent = false;
	}

	auto APawn::Tick(float DeltaSeconds) -> void
	{
		if (bHasPendingControlIntent)
		{
			const FPawnControlIntent Intent = PendingControlIntent;
			ClearPendingControlIntent();
			if (MovementComponent && MovementComponent->GetPawnOwner() == this)
			{
				MovementComponent->PerformMovement(Intent, DeltaSeconds);
			}
		}
		Super::Tick(DeltaSeconds);
	}

	auto APawn::SetMovementComponent(DPawnMovementComponent* Component) -> bool
	{
		if (!Component)
		{
			MovementComponent = nullptr;
			return true;
		}
		if (MovementComponent || Component->GetOwner() != this || Component->IsBeingDestroyed()) return false;
		MovementComponent = Component;
		return true;
	}

	auto APawn::EndPlay() -> void
	{
		if (AController* ExistingController = Controller.Get())
		{
			AController::DetachPair(ExistingController, this);
			ExistingController->OnPossessedPawnChanged(this, nullptr);
		}
		ClearPendingControlIntent();
		Super::EndPlay();
	}

	auto APawn::OnActorDestroyed() -> void
	{
		if (AController* ExistingController = Controller.Get())
		{
			AController::DetachPair(ExistingController, this);
			ExistingController->OnPossessedPawnChanged(this, nullptr);
		}
		ClearPendingControlIntent();
		Super::OnActorDestroyed();
	}
}
