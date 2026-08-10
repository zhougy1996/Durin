#include "Actors/Controller.h"

#include "Actors/Pawn.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	AController::AController(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	void AController::DetachPair(AController* Controller, APawn* Pawn)
	{
		if (Pawn)
		{
			Pawn->ClearPendingControlIntent();
			if (!Controller || Pawn->Controller.Get() == Controller) Pawn->Controller = nullptr;
		}
		if (Controller && (!Pawn || Controller->Pawn.Get() == Pawn)) Controller->Pawn = nullptr;
	}

	auto AController::Possess(APawn* TargetPawn) -> FPossessionResult
	{
		if (!TargetPawn) return {EPossessionError::NullPawn, "Possession requires a pawn."};
		if (IsPendingKill() || IsBeingDestroyed()) return {EPossessionError::ControllerUnavailable, "The controller is being destroyed."};
		if (TargetPawn->IsPendingKill() || TargetPawn->IsBeingDestroyed()) return {EPossessionError::PawnUnavailable, "The pawn is being destroyed."};

		auto* ControllerLevel = Cast<DLevel>(GetOuter());
		auto* PawnLevel = Cast<DLevel>(TargetPawn->GetOuter());
		DWorld* World = ControllerLevel ? ControllerLevel->GetWorld() : nullptr;
		if (!ControllerLevel || ControllerLevel != PawnLevel || !World
			|| World->GetCurrentLevel() != ControllerLevel
			|| !ControllerLevel->ContainsActor(this)
			|| !ControllerLevel->ContainsActor(TargetPawn))
		{
			return {EPossessionError::InvalidMembership, "Controller and pawn must be live actors in the same active World level."};
		}
		if (World->IsEndingPlay()) return {EPossessionError::WorldEnding, "Possession is unavailable while the World is ending play."};
		if (Pawn.Get() == TargetPawn && TargetPawn->Controller.Get() == this) return {};

		APawn* PreviousPawn = Pawn.Get();
		AController* PreviousController = TargetPawn->Controller.Get();
		DetachPair(this, PreviousPawn);
		if (PreviousController && PreviousController != this)
		{
			DetachPair(PreviousController, TargetPawn);
			PreviousController->OnPossessedPawnChanged(TargetPawn, nullptr);
		}
		Pawn = TargetPawn;
		TargetPawn->Controller = this;
		OnPossessedPawnChanged(PreviousPawn, TargetPawn);
		return {};
	}

	auto AController::UnPossess() -> void
	{
		APawn* PreviousPawn = Pawn.Get();
		if (!PreviousPawn) return;
		DetachPair(this, PreviousPawn);
		OnPossessedPawnChanged(PreviousPawn, nullptr);
	}

	auto AController::SubmitControlIntent(const FPawnControlIntent& Intent) -> bool
	{
		return Pawn && Pawn->AdmitControlIntent(Intent);
	}

	auto AController::OnPossessedPawnChanged(APawn*, APawn*) -> void
	{
	}

	auto AController::EndPlay() -> void
	{
		UnPossess();
		Super::EndPlay();
	}

	auto AController::OnActorDestroyed() -> void
	{
		UnPossess();
		Super::OnActorDestroyed();
	}
}
