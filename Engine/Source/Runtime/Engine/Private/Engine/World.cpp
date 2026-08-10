#include "Engine/World.h"

#include "Actors/GameMode.h"
#include "Actors/Pawn.h"
#include "Actors/PlayerController.h"
#include "Actors/PlayerStart.h"
#include "Components/ActorComponent.h"
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
			for (const TObjectPtr<AActor>& Actor : Previous->GetActors())
			{
				if (auto* Controller = Cast<AController>(Actor.Get())) Controller->UnPossess();
			}
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
				if (Component && Component->IsRegistered()) Component->UnregisterComponent();
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
			if (Component && Component->IsRegistered()) Component->UnregisterComponent();
		}

		RenderScene = InRenderScene;
		for (const TObjectPtr<DActorComponent>& Component : RegisteredComponents)
		{
			if (Component && !Component->IsPendingKill()) Component->RegisterComponent();
		}
	}

	auto DWorld::BeginPlay(const FWorldPlayRequest& Request) -> FWorldPlayResult
	{
		if (PlayState != EWorldPlayState::Stopped)
			return PlayFailure(EWorldPlayError::InvalidState, "The World must be stopped before play begins.");
		if (!CurrentLevel)
			return PlayFailure(EWorldPlayError::MissingLevel, "The World has no active level.");

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
		}

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
		return {};
	}

	auto DWorld::Tick(const FWorldTickContext& Context) -> void
	{
		if (!HasBegunPlay() || !CurrentLevel) return;
		DLevel* CapturedLevel = CurrentLevel.Get();
		if (bPaused && !std::exchange(bSingleStepRequested, false))
		{
			ClearPendingGameplayIntent();
			return;
		}
		if (GameplaySession && GameplaySession->LocalPlayerController && Context.GameInput)
		{
			GameplaySession->LocalPlayerController->PreparePlayerInput(*Context.GameInput);
		}
		if (!HasBegunPlay() || CurrentLevel.Get() != CapturedLevel) return;
		const std::vector<TObjectPtr<AActor>> Actors = CapturedLevel->GetActors();
		for (const TObjectPtr<AActor>& Actor : Actors)
		{
			if (!HasBegunPlay() || CurrentLevel.Get() != CapturedLevel) break;
			if (Actor
				&& !Actor->IsPendingKill()
				&& Actor->GetOuter() == CapturedLevel
				&& !Actor->IsBeingDestroyed()
				&& Actor->HasBegunPlay()
				&& Actor->IsActorTickEnabled())
			{
				Actor->Tick(Context.DeltaSeconds);
			}
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
		if (PlayState == EWorldPlayState::EndingPlay) PlayState = EWorldPlayState::Stopped;
	}

	auto DWorld::RestartPlayer(const FPlayerRestartRequest& Request) -> FPlayerRestartResult
	{
		if (!GameplaySession || PlayState != EWorldPlayState::Playing || !CurrentLevel)
			return RestartFailure(EPlayerRestartError::NoGameplaySession, "The World has no active native gameplay session.");
		APlayerController* Controller = GameplaySession->LocalPlayerController.Get();
		AGameMode* GameMode = GameplaySession->GameMode.Get();
		if (!Controller || !CurrentLevel->ContainsActor(Controller))
			return RestartFailure(EPlayerRestartError::ControllerUnavailable, "The local player controller is unavailable.");
		if (!GameMode || !CurrentLevel->ContainsActor(GameMode))
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
		if (!CurrentLevel->ContainsActor(Pawn))
			return RestartFailure(EPlayerRestartError::PawnSpawnFailed, "The replacement pawn was destroyed during BeginPlay.");
		return {EPlayerRestartError::None, {}, Pawn};
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

	auto DWorld::OnActorDestroyed(AActor* Actor) -> void
	{
		if (!Actor) return;
		if (CurrentLevel)
		{
			const std::vector<TObjectPtr<AActor>> Actors = CurrentLevel->GetActors();
			for (const TObjectPtr<AActor>& Candidate : Actors)
			{
				if (auto* PlayerController = Cast<APlayerController>(Candidate.Get()))
					PlayerController->HandleViewTargetDestroyed(Actor);
			}
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
} // namespace Durin
