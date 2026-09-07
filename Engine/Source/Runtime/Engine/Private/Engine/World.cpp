#include "Engine/World.h"
#include "WorldOperation.h"

#include "Actors/GameMode.h"
#include "Actors/Pawn.h"
#include "Actors/PlayerController.h"
#include "Actors/PlayerStart.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Input/GameInputState.h"

namespace Durin
{
	namespace
	{
		auto PlayFailure(EWorldPlayError Error, std::string Message) -> FWorldPlayResult
		{
			return {Error, std::move(Message)};
		}

		auto RestartFailure(EPlayerRestartError Error, std::string Message) -> FPlayerRestartResult
		{
			return {Error, std::move(Message), nullptr};
		}
	}

	auto DWorld::BeginPlay(const FWorldPlayRequest& Request) -> FWorldPlayResult
	{
		if (Operation != EOperation::Idle)
			return PlayFailure(EWorldPlayError::InvalidState, "World operation is in progress.");
		FOperationScope OperationScope(*this, EOperation::BeginningPlay);
		return BeginPlayInternal(Request);
	}

	auto DWorld::BeginPlayInternal(const FWorldPlayRequest& Request) -> FWorldPlayResult
	{
		if (!CanDispatchSubsystems() || PlayState != EWorldPlayState::Stopped)
			return PlayFailure(EWorldPlayError::InvalidState, "The World must be stopped before play begins.");
		if (!CurrentLevel)
			return PlayFailure(EWorldPlayError::MissingLevel, "The World has no active level.");

		AGameMode* ExpectedGameMode = nullptr;
		APlayerController* ExpectedController = nullptr;
		APawn* ExpectedPawn = nullptr;
		if (Request.GameModeClass)
		{
			if (!CanConstructObjectOfClass(Request.GameModeClass, AGameMode::StaticClass()))
				return PlayFailure(EWorldPlayError::InvalidGameModeClass, "The requested game-mode class is not a constructible AGameMode.");

			DLevel* Level = CurrentLevel.Get();
			std::vector<TObjectPtr<AActor>> CreatedActors;
			auto Rollback = [&] {
				for (auto It = CreatedActors.rbegin(); It != CreatedActors.rend(); ++It)
				{
					if (*It && Level->ContainsActor(It->Get())) Level->DestroyActor(It->Get());
				}
				GameplaySession.reset();
				PlayState = EWorldPlayState::Stopped;
			};

			auto* GameMode = Cast<AGameMode>(Level->SpawnActor(Request.GameModeClass, "GameMode"));
			if (!GameMode) return PlayFailure(EWorldPlayError::GameModeSpawnFailed, "Could not spawn the requested game mode.");
			CreatedActors.emplace_back(GameMode);
			DClass* ControllerClass = GameMode->GetPlayerControllerClass();
			if (!CanConstructObjectOfClass(ControllerClass, APlayerController::StaticClass()))
			{
				Rollback();
				return PlayFailure(EWorldPlayError::InvalidPlayerControllerClass, "The game mode selected an invalid player-controller class.");
			}
			DClass* PawnClass = GameMode->GetDefaultPawnClass();
			if (!CanConstructObjectOfClass(PawnClass, APawn::StaticClass()))
			{
				Rollback();
				return PlayFailure(EWorldPlayError::InvalidPawnClass, "The game mode selected an invalid default-pawn class.");
			}
			APlayerStart* PlayerStart = GameMode->ChoosePlayerStart(*this);
			if (!PlayerStart || !Level->ContainsActor(PlayerStart))
			{
				Rollback();
				return PlayFailure(EWorldPlayError::MissingPlayerStart, "The active level has no valid player start.");
			}

			auto* Controller = Cast<APlayerController>(Level->SpawnActor(ControllerClass, "LocalPlayerController"));
			if (!Controller)
			{
				Rollback();
				return PlayFailure(EWorldPlayError::PlayerControllerSpawnFailed, "Could not spawn the local player controller.");
			}
			CreatedActors.emplace_back(Controller);
			auto* Pawn = Cast<APawn>(Level->SpawnActor(PawnClass, "DefaultPawn"));
			if (!Pawn)
			{
				Rollback();
				return PlayFailure(EWorldPlayError::PawnSpawnFailed, "Could not spawn the default pawn.");
			}
			CreatedActors.emplace_back(Pawn);
			if (!Pawn->SetActorTransform(PlayerStart->GetActorTransform()))
			{
				Rollback();
				return PlayFailure(EWorldPlayError::PawnPlacementFailed, "Could not place the default pawn at the selected player start.");
			}
			const FPossessionResult Possession = Controller->Possess(Pawn);
			if (!Possession)
			{
				Rollback();
				return PlayFailure(EWorldPlayError::PossessionFailed, std::format("Could not possess the default pawn: {}", Possession.Message));
			}
			if (Request.ViewTargetOverride)
			{
				const FViewTargetResult ViewResult = Controller->SetViewTarget(Request.ViewTargetOverride);
				if (!ViewResult)
				{
					Rollback();
					return PlayFailure(EWorldPlayError::ViewTargetRejected, std::format("Could not apply the requested view target: {}", ViewResult.Message));
				}
			}

			GameplaySession = FNativeGameplaySession{
				.GameMode = GameMode,
				.LocalPlayerController = Controller,
				.DefaultPawn = Pawn,
				.RuntimeActors = CreatedActors};
			ExpectedGameMode = GameMode;
			ExpectedController = Controller;
			ExpectedPawn = Pawn;
		}

		DLevel* CapturedLevel = CurrentLevel.Get();
		PlayState = EWorldPlayState::BeginningPlay;
		bBeginningSubsystemPlay = true;
		Subsystems.BeginPlay();
		bBeginningSubsystemPlay = false;
		if (!CanDispatchSubsystems() || PlayState != EWorldPlayState::BeginningPlay)
		{
			EndPlayInternal();
			return PlayFailure(EWorldPlayError::PlayAborted, "Subsystem BeginPlay interrupted play.");
		}
		const std::vector<TObjectPtr<AActor>> Actors = CapturedLevel->GetActors();
		for (const TObjectPtr<AActor>& Actor : Actors)
		{
			if (!CanDispatchSubsystems() || PlayState != EWorldPlayState::BeginningPlay || CurrentLevel.Get() != CapturedLevel) break;
			if (Actor
				&& !Actor->IsPendingKill()
				&& Actor->GetOuter() == CapturedLevel
				&& !Actor->IsBeingDestroyed()
				&& !Actor->HasBegunPlay())
			{
				Actor->DispatchBeginPlay();
			}
		}
		if (!CanDispatchSubsystems() || PlayState != EWorldPlayState::BeginningPlay || CurrentLevel.Get() != CapturedLevel)
		{
			EndPlayInternal();
			return PlayFailure(EWorldPlayError::PlayAborted, "Play was interrupted by a gameplay callback.");
		}
		if (Request.GameModeClass
			&& (!GameplaySession
				|| GameplaySession->GameMode.Get() != ExpectedGameMode
				|| GameplaySession->LocalPlayerController.Get() != ExpectedController
				|| GameplaySession->DefaultPawn.Get() != ExpectedPawn
				|| !CapturedLevel->ContainsActor(ExpectedGameMode)
				|| !CapturedLevel->ContainsActor(ExpectedController)
				|| !CapturedLevel->ContainsActor(ExpectedPawn)
				|| ExpectedController->GetPawn() != ExpectedPawn
				|| ExpectedPawn->GetController() != ExpectedController))
		{
			EndPlayInternal();
			return PlayFailure(EWorldPlayError::PlayAborted, "Native gameplay bootstrap was invalidated by a BeginPlay callback.");
		}
		PlayState = EWorldPlayState::Playing;
		return {};
	}

	auto DWorld::Tick(const FWorldTickContext& Context) -> void
	{
		if (Operation != EOperation::Idle || bShutdownRequested || GetSubsystemState() != EWorldSubsystemState::Ready) return;
		ProcessPendingLevelTransition();
		if (!CanDispatchSubsystems()) return;
		FOperationScope OperationScope(*this, EOperation::Ticking);
		Subsystems.StartTick();
		DLevel* CapturedLevel = CurrentLevel.Get();
		bool bGameplay = HasBegunPlay() && CapturedLevel;
		if (bGameplay && bPaused && !std::exchange(bSingleStepRequested, false))
		{
			ClearPendingGameplayIntent();
			bGameplay = false;
		}
		if (bGameplay && GameplaySession && GameplaySession->LocalPlayerController && Context.GameInput)
			GameplaySession->LocalPlayerController->PreparePlayerInput(*Context.GameInput);
		if (!CanDispatchSubsystems() || (bGameplay && !CanContinueTicking(CapturedLevel))) return;
		// Registry cleanup must precede the operation scope applying stop requests.
		struct FTickFrameScope
		{
			FTickRegistry* Registry;
			~FTickFrameScope() { if (Registry) Registry->EndFrame(); }
		};
		if (bGameplay) CapturedLevel->TickRegistry.StartFrame(Context.DeltaSeconds);
		const FTickFrameScope FrameScope{bGameplay ? &CapturedLevel->TickRegistry : nullptr};
		for (const ETickingGroup Group : {ETickingGroup::PrePhysics, ETickingGroup::Physics, ETickingGroup::PostPhysics})
		{
			if (!CanDispatchSubsystems() || (bGameplay && !CanContinueTicking(CapturedLevel))) break;
			Subsystems.Tick(Group, Context.DeltaSeconds, bGameplay);
			if (!CanDispatchSubsystems()) break;
			if (bGameplay && (!CanContinueTicking(CapturedLevel) || !CapturedLevel->TickRegistry.RunTickGroup(Group))) break;
		}
	}

	auto DWorld::CanContinueTicking(const DLevel* Level) const -> bool
	{
		return CanDispatchSubsystems() && HasBegunPlay()
			&& CurrentLevel.Get() == Level
			&& !PendingLevelTransition;
	}

	auto DWorld::EndPlay() -> void
	{
		if (PlayState == EWorldPlayState::EndingPlay) return;
		if (Operation != EOperation::Idle) { bEndPlayRequested = true; return; }
		FOperationScope OperationScope(*this, EOperation::EndingPlay);
		EndPlayInternal();
	}

	auto DWorld::EndPlayInternal() -> void
	{
		bEndPlayRequested = false;
		if (PlayState == EWorldPlayState::Stopped || PlayState == EWorldPlayState::EndingPlay) return;
		DLevel* CapturedLevel = CurrentLevel.Get();
		std::vector<TObjectPtr<AActor>> Actors;
		if (CapturedLevel) Actors = CapturedLevel->GetActors();
		PlayState = EWorldPlayState::EndingPlay;
		bSingleStepRequested = false;
		ClearPendingGameplayIntent();
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
		if (GameplaySession && CapturedLevel)
		{
			const std::vector<TObjectPtr<AActor>> RuntimeActors = GameplaySession->RuntimeActors;
			for (auto It = RuntimeActors.rbegin(); It != RuntimeActors.rend(); ++It)
			{
				if (*It && CapturedLevel->ContainsActor(It->Get())) CapturedLevel->DestroyActor(It->Get());
			}
		}
		GameplaySession.reset();
		Subsystems.EndPlay();
		if (PlayState == EWorldPlayState::EndingPlay) PlayState = EWorldPlayState::Stopped;
	}

	auto DWorld::RestartPlayer(const FPlayerRestartRequest& Request) -> FPlayerRestartResult
	{
		if (Operation != EOperation::Idle || !GameplaySession || PlayState != EWorldPlayState::Playing || !CurrentLevel)
			return RestartFailure(EPlayerRestartError::NoGameplaySession, "The World has no active native gameplay session.");
		FOperationScope OperationScope(*this, EOperation::RestartingPlayer);
		DLevel* Level = CurrentLevel.Get();
		APlayerController* Controller = GameplaySession->LocalPlayerController.Get();
		AGameMode* GameMode = GameplaySession->GameMode.Get();
		if (!Controller || !Level->ContainsActor(Controller))
			return RestartFailure(EPlayerRestartError::ControllerUnavailable, "The local player controller is unavailable.");
		if (!GameMode || !Level->ContainsActor(GameMode))
			return RestartFailure(EPlayerRestartError::GameModeUnavailable, "The active game mode is unavailable.");

		Controller->UnPossess();
		if (APawn* PreviousPawn = GameplaySession->DefaultPawn.Get(); PreviousPawn && CurrentLevel->ContainsActor(PreviousPawn))
			CurrentLevel->DestroyActor(PreviousPawn);
		GameplaySession->DefaultPawn = nullptr;

		DClass* PawnClass = GameMode->GetDefaultPawnClass();
		if (!CanConstructObjectOfClass(PawnClass, APawn::StaticClass()))
			return RestartFailure(EPlayerRestartError::InvalidPawnClass, "The game mode selected an invalid default-pawn class.");
		APlayerStart* PlayerStart = GameMode->ChoosePlayerStart(*this);
		if (!PlayerStart || !CurrentLevel->ContainsActor(PlayerStart))
			return RestartFailure(EPlayerRestartError::MissingPlayerStart, "The active level has no valid player start.");
		auto* Pawn = Cast<APawn>(CurrentLevel->SpawnActorDeferredPlay(PawnClass, "DefaultPawn"));
		if (!Pawn) return RestartFailure(EPlayerRestartError::PawnSpawnFailed, "Could not spawn the replacement pawn.");
		GameplaySession->RuntimeActors.emplace_back(Pawn);
		if (!Pawn->SetActorTransform(PlayerStart->GetActorTransform()))
		{
			CurrentLevel->DestroyActor(Pawn);
			return RestartFailure(EPlayerRestartError::PawnPlacementFailed, "Could not place the replacement pawn.");
		}
		const FPossessionResult Possession = Controller->Possess(Pawn);
		if (!Possession)
		{
			CurrentLevel->DestroyActor(Pawn);
			return RestartFailure(EPlayerRestartError::PossessionFailed, std::format("Could not possess the replacement pawn: {}", Possession.Message));
		}
		if (Request.ViewTargetOverride)
		{
			const FViewTargetResult ViewResult = Controller->SetViewTarget(Request.ViewTargetOverride);
			if (!ViewResult)
			{
				Controller->UnPossess();
				CurrentLevel->DestroyActor(Pawn);
				return RestartFailure(EPlayerRestartError::ViewTargetRejected, std::format("Could not apply the replacement view target: {}", ViewResult.Message));
			}
		}
		GameplaySession->DefaultPawn = Pawn;
		Pawn->DispatchBeginPlay();
		if (!CanDispatchSubsystems() || PlayState != EWorldPlayState::Playing
			|| CurrentLevel.Get() != Level
			|| !GameplaySession
			|| GameplaySession->GameMode.Get() != GameMode
			|| GameplaySession->LocalPlayerController.Get() != Controller
			|| PendingLevelTransition)
		{
			return RestartFailure(EPlayerRestartError::RestartAborted, "Player restart was superseded by a gameplay lifecycle transition.");
		}
		if (GameplaySession->DefaultPawn.Get() != Pawn || !Level->ContainsActor(Pawn))
			return RestartFailure(EPlayerRestartError::PawnSpawnFailed, "The replacement pawn was destroyed during BeginPlay.");
		if (!Pawn->HasBegunPlay() || Controller->GetPawn() != Pawn || Pawn->GetController() != Controller)
			return RestartFailure(EPlayerRestartError::RestartAborted, "Player restart relationships were invalidated during BeginPlay.");
		return {EPlayerRestartError::None, {}, Pawn};
	}

	auto DWorld::ProcessPendingLevelTransition() -> void
	{
		if (!PendingLevelTransition || Operation != EOperation::Idle) return;
		FOperationScope OperationScope(*this, EOperation::ChangingLevel);
		ActiveLevelTransition = std::move(PendingLevelTransition);
		PendingLevelTransition.reset();
		// Active ownership remains visible to GC across every transition callback.
		const FPendingLevelTransition Transition = *ActiveLevelTransition;
		EndPlayInternal();
		if (!bShutdownRequested && !IsPendingKill()
			&& SetCurrentLevelInternal(Transition.Level.Get(), Transition.bDestroyPreviousOwnedLevel)
			&& Transition.Level && Transition.bResumePlay && CanDispatchSubsystems())
			(void)BeginPlayInternal({.GameModeClass = Transition.GameModeClass});
		ActiveLevelTransition.reset();
	}

	auto DWorld::SetPaused(bool bInPaused) -> void
	{
		if (bPaused == bInPaused) return;
		bPaused = bInPaused;
		bSingleStepRequested = false;
		ClearPendingGameplayIntent();
	}

	auto DWorld::GetGameMode() const -> AGameMode*
	{
		return GameplaySession && GameplaySession->GameMode ? GameplaySession->GameMode.Get() : nullptr;
	}

	auto DWorld::GetLocalPlayerController() const -> APlayerController*
	{
		return GameplaySession && GameplaySession->LocalPlayerController ? GameplaySession->LocalPlayerController.Get() : nullptr;
	}

	auto DWorld::GetDefaultPawn() const -> APawn*
	{
		return GameplaySession && GameplaySession->DefaultPawn ? GameplaySession->DefaultPawn.Get() : nullptr;
	}

} // namespace Durin
