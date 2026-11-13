#include "Actors/PlayerController.h"

#include "Actors/Pawn.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Input/GameInputState.h"

namespace Durin
{
	APlayerController::APlayerController(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto APlayerController::SetViewTarget(AActor* Target) -> void
	{
		if (!Target)
		{
			ViewTarget = nullptr;
			return;
		}
		if (IsPendingKill() || IsBeingDestroyed()) { DURIN_WARN("The player controller is being destroyed."); return; }
		if (Target->IsPendingKill() || Target->IsBeingDestroyed()) { DURIN_WARN("The requested view target is being destroyed."); return; }
		auto* ControllerLevel = Cast<DLevel>(GetOuter());
		auto* TargetLevel = Cast<DLevel>(Target->GetOuter());
		DWorld* World = ControllerLevel ? ControllerLevel->GetWorld() : nullptr;
		if (!ControllerLevel || ControllerLevel != TargetLevel || !World
			|| World->GetCurrentLevel() != ControllerLevel
			|| !ControllerLevel->ContainsActor(this)
			|| !ControllerLevel->ContainsActor(Target))
		{
			DURIN_WARN("The view target must be a live actor in the controller's active World level.");
			return;
		}
		if (World->IsEndingPlay()) { DURIN_WARN("View targets cannot change while the World is ending play."); return; }
		ViewTarget = Target;
	}

	auto APlayerController::BuildControlIntent(const FInputActionSnapshot&) const -> FPawnControlIntent
	{
		return {};
	}

	auto APlayerController::OnPossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn) -> void
	{
		CancelPlayerInput();
		Super::OnPossessedPawnChanged(PreviousPawn, NewPawn);
		ViewTarget = NewPawn;
	}

	auto APlayerController::PreparePlayerInput(const FGameInputState& Input) -> void
	{
		InputActions.SetDeviceBlocked(Input.IsKeyboardBlocked(), Input.IsMouseBlocked());
		SubmitControlIntent(BuildControlIntent(InputActions.Evaluate(Input)));
	}

	auto APlayerController::CancelPlayerInput() -> void
	{
		InputActions.Cancel();
		if (APawn* ControlledPawn = GetPawn()) ControlledPawn->ClearPendingControlIntent();
	}

	auto APlayerController::EndPlay() -> void
	{
		CancelPlayerInput();
		ViewTarget = nullptr;
		Super::EndPlay();
	}

	auto APlayerController::OnActorDestroyed() -> void
	{
		CancelPlayerInput();
		ViewTarget = nullptr;
		Super::OnActorDestroyed();
	}

	auto APlayerController::HandleViewTargetDestroyed(AActor* Target) -> void
	{
		if (ViewTarget.Get() == Target) ViewTarget = nullptr;
	}
}
