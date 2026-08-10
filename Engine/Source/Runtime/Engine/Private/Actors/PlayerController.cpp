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

	auto APlayerController::SetViewTarget(AActor* Target) -> FViewTargetResult
	{
		if (!Target)
		{
			ViewTarget = nullptr;
			return {};
		}
		if (IsPendingKill() || IsBeingDestroyed()) return {EViewTargetError::ControllerUnavailable, "The player controller is being destroyed."};
		if (Target->IsPendingKill() || Target->IsBeingDestroyed()) return {EViewTargetError::TargetUnavailable, "The requested view target is being destroyed."};
		auto* ControllerLevel = Cast<DLevel>(GetOuter());
		auto* TargetLevel = Cast<DLevel>(Target->GetOuter());
		DWorld* World = ControllerLevel ? ControllerLevel->GetWorld() : nullptr;
		if (!ControllerLevel || ControllerLevel != TargetLevel || !World
			|| World->GetCurrentLevel() != ControllerLevel
			|| !ControllerLevel->ContainsActor(this)
			|| !ControllerLevel->ContainsActor(Target))
		{
			return {EViewTargetError::InvalidMembership, "The view target must be a live actor in the controller's active World level."};
		}
		if (World->IsEndingPlay()) return {EViewTargetError::WorldEnding, "View targets cannot change while the World is ending play."};
		ViewTarget = Target;
		return {};
	}

	auto APlayerController::BuildControlIntent(const FGameInputState&) const -> FPawnControlIntent
	{
		return {};
	}

	auto APlayerController::OnPossessedPawnChanged(APawn* PreviousPawn, APawn* NewPawn) -> void
	{
		Super::OnPossessedPawnChanged(PreviousPawn, NewPawn);
		ViewTarget = NewPawn;
	}

	auto APlayerController::PreparePlayerInput(const FGameInputState& Input) -> void
	{
		SubmitControlIntent(BuildControlIntent(Input));
	}

	auto APlayerController::EndPlay() -> void
	{
		ViewTarget = nullptr;
		Super::EndPlay();
	}

	auto APlayerController::OnActorDestroyed() -> void
	{
		ViewTarget = nullptr;
		Super::OnActorDestroyed();
	}

	auto APlayerController::HandleViewTargetDestroyed(AActor* Target) -> void
	{
		if (ViewTarget.Get() == Target) ViewTarget = nullptr;
	}
}
