#include "Engine/World.h"

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
		auto MakePhysicsQueryFilter(
			ECollisionChannel TraceChannel,
			const FCollisionQueryParams& QueryParams,
			const FCollisionResponseParams& ResponseParams) -> FPhysicsQueryFilter
		{
			FPhysicsQueryFilter Result;
			Result.QueryChannel = static_cast<uint8>(TraceChannel);
			for (uint8 Index = 0; Index < MaximumPhysicsChannels; ++Index)
				Result.Responses[Index] = ToPhysicsResponse(ResponseParams.CollisionResponse.Responses[Index]);
			for (const DPrimitiveComponent* Component : QueryParams.IgnoredComponents)
			{
				if (Component && Component->GetPhysicsActorHandle().IsValid())
					Result.IgnoredActors.push_back(Component->GetPhysicsActorHandle());
			}
			return Result;
		}

		auto IsIgnoredByOwner(
			const DPrimitiveComponent* Component,
			const FCollisionQueryParams& QueryParams) -> bool
		{
			return Component && std::ranges::find(
				QueryParams.IgnoredActors, Component->GetOwner()) != QueryParams.IgnoredActors.end();
		}

		auto MapPhysicsHit(const FPhysicsQueryHit& Source, FHitResult& OutHit) -> bool
		{
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Source.UserToken);
			if (!Component) return false;
			OutHit.bBlockingHit = Source.Response == EPhysicsQueryResponse::Block;
			OutHit.bStartPenetrating = Source.bStartPenetrating;
			OutHit.Time = Source.Time;
			OutHit.Distance = Source.Distance;
			OutHit.Location = Source.Location;
			OutHit.ImpactPoint = Source.ImpactPoint;
			OutHit.ImpactNormal = Source.ImpactNormal;
			OutHit.PenetrationDepth = Source.PenetrationDepth;
			OutHit.Component = Component;
			OutHit.Actor = Component->GetOwner();
			return true;
		}

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
				for (const TObjectPtr<DActorComponent>& Component : Actor->GetOwnedComponents())
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

	auto DWorld::RequestLevelTransition(DLevel* Level, bool bDestroyPreviousOwnedLevel) -> bool
	{
		if (Level && Level->GetWorld() && Level->GetWorld() != this) return false;
		if (Level && Cast<DWorld>(Level->GetOuter()) && Level->GetOuter() != this) return false;
		PendingLevelTransition = FPendingLevelTransition{
			.Level = Level,
			.GameModeClass = GameplaySession && GameplaySession->GameMode
				? GameplaySession->GameMode->GetClass()
				: nullptr,
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
		if (PlayState != EWorldPlayState::BeginningPlay || CurrentLevel.Get() != CapturedLevel)
			return PlayFailure(EWorldPlayError::PlayAborted, "Play was interrupted by a gameplay callback.");
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
			EndPlay();
			return PlayFailure(EWorldPlayError::PlayAborted, "Native gameplay bootstrap was invalidated by a BeginPlay callback.");
		}
		PlayState = EWorldPlayState::Playing;
		return {};
	}

	auto DWorld::Tick(const FWorldTickContext& Context) -> void
	{
		ProcessPendingLevelTransition();
		if (!HasBegunPlay() || !CurrentLevel || PendingLevelTransition) return;
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
		if (!HasBegunPlay() || CurrentLevel.Get() != CapturedLevel || PendingLevelTransition) return;
		CapturedLevel->TickRegistry.StartFrame(Context.DeltaSeconds);
		for (const ETickingGroup Group : {ETickingGroup::PrePhysics, ETickingGroup::Physics, ETickingGroup::PostPhysics})
		{
			if (!CanContinueTicking(CapturedLevel) || !CapturedLevel->TickRegistry.RunTickGroup(Group)) break;
		}
		CapturedLevel->TickRegistry.EndFrame();
	}

	auto DWorld::CanContinueTicking(const DLevel* Level) const -> bool
	{
		return HasBegunPlay()
			&& CurrentLevel.Get() == Level
			&& !PendingLevelTransition;
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
		if (PlayState != EWorldPlayState::Playing
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
		if (!PendingLevelTransition) return;
		const FPendingLevelTransition Transition = *PendingLevelTransition;
		PendingLevelTransition.reset();
		EndPlay();
		if (!SetCurrentLevel(Transition.Level.Get(), Transition.bDestroyPreviousOwnedLevel)) return;
		if (Transition.Level && Transition.bResumePlay)
			(void)BeginPlay({.GameModeClass = Transition.GameModeClass});
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

	auto DWorld::LineTraceSingleByChannel(
		FHitResult& OutHit,
		const FVector3& Start,
		const FVector3& End,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams,
		const FCollisionResponseParams& ResponseParams) const -> bool
	{
		OutHit.Reset();
		FPhysicsQueryFilter Filter = MakePhysicsQueryFilter(TraceChannel, QueryParams, ResponseParams);
		while (true)
		{
			FPhysicsQueryHit Hit;
			if (!PhysicsScene.LineTraceSingle(Start, End, Filter, Hit)) return false;
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Hit.UserToken);
			if (!IsIgnoredByOwner(Component, QueryParams))
			{
				const bool bMapped = MapPhysicsHit(Hit, OutHit);
				if (bMapped && bCollisionDebugDrawEnabled) LastCollisionDebugHit = OutHit;
				return bMapped;
			}
			Filter.IgnoredActors.push_back(Hit.ActorHandle);
		}
	}

	auto DWorld::SweepSingleByChannel(
		FHitResult& OutHit,
		const FCollisionShape& Shape,
		const FTransform& StartTransform,
		const FVector3& Delta,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams,
		const FCollisionResponseParams& ResponseParams) const -> bool
	{
		OutHit.Reset();
		FPhysicsQueryFilter Filter = MakePhysicsQueryFilter(TraceChannel, QueryParams, ResponseParams);
		while (true)
		{
			FPhysicsQueryHit Hit;
			if (!PhysicsScene.SweepSingle(Shape, StartTransform, Delta, Filter, Hit)) return false;
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Hit.UserToken);
			if (!IsIgnoredByOwner(Component, QueryParams))
			{
				const bool bMapped = MapPhysicsHit(Hit, OutHit);
				if (bMapped && bCollisionDebugDrawEnabled) LastCollisionDebugHit = OutHit;
				return bMapped;
			}
			Filter.IgnoredActors.push_back(Hit.ActorHandle);
		}
	}

	auto DWorld::OverlapMultiByChannel(
		std::vector<FOverlapResult>& OutOverlaps,
		const FCollisionShape& Shape,
		const FTransform& Transform,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams,
		const FCollisionResponseParams& ResponseParams) const -> bool
	{
		OutOverlaps.clear();
		std::vector<FPhysicsQueryHit> Hits;
		if (!PhysicsScene.OverlapMulti(
			Shape, Transform, MakePhysicsQueryFilter(TraceChannel, QueryParams, ResponseParams), Hits)) return false;
		for (const FPhysicsQueryHit& Hit : Hits)
		{
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Hit.UserToken);
			if (!Component || IsIgnoredByOwner(Component, QueryParams)) continue;
			OutOverlaps.push_back({Component->GetOwner(), Component, Hit.Response == EPhysicsQueryResponse::Block});
		}
		return !OutOverlaps.empty();
	}

	auto DWorld::SetCollisionDebugDrawEnabled(bool bEnabled) -> void
	{
		bCollisionDebugDrawEnabled = bEnabled;
		if (!bEnabled) LastCollisionDebugHit.reset();
	}

	auto DWorld::CaptureCollisionDebugSnapshot() const -> FCollisionDebugSnapshot
	{
		constexpr size_t MaximumDebugBodies = 4096;
		constexpr uint32 MaximumDebugTriangles = 256;
		FCollisionDebugSnapshot Result;
		uint32 RemainingDebugTriangles = MaximumDebugTriangles;
		if (!bCollisionDebugDrawEnabled) return Result;
		for (const FPhysicsBodySnapshot& Body : PhysicsScene.CaptureBodies())
		{
			if (Result.Bodies.size() >= MaximumDebugBodies) break;
			auto* Component = reinterpret_cast<DPrimitiveComponent*>(Body.Desc.UserToken);
			if (!Component) continue;
			FCollisionDebugBody DebugBody;
			DebugBody.Handle = Body.Handle;
			DebugBody.GeometryKind = Body.Desc.Geometry.GetKind();
			DebugBody.Transform = Body.Desc.Transform;
			DebugBody.ObjectChannel = static_cast<ECollisionChannel>(Body.Desc.Filter.ObjectChannel);
			DebugBody.Actor = Component->GetOwner();
			DebugBody.Component = Component;
			if (const FCollisionGeometryChild* Child = Body.Desc.Geometry.GetChild(0))
			{
				DebugBody.Shape = Child->Shape;
				DebugBody.bHasPrimitiveShape = true;
			}
			Body.Desc.Geometry.GetLocalBounds(
				DebugBody.LocalBoundsMinimum, DebugBody.LocalBoundsMaximum);
			DebugBody.TotalTriangles = Body.Desc.Geometry.GetTriangleCount();
			const uint32 SampleCount = std::min(DebugBody.TotalTriangles, RemainingDebugTriangles);
			DebugBody.TriangleSample.reserve(SampleCount);
			for (uint32 Index = 0; Index < SampleCount; ++Index)
			{
				const FCollisionGeometryTriangle* Triangle = Body.Desc.Geometry.GetTriangle(Index);
				if (!Triangle) break;
				const FVector3* First = Body.Desc.Geometry.GetVertex(Triangle->First);
				const FVector3* Second = Body.Desc.Geometry.GetVertex(Triangle->Second);
				const FVector3* Third = Body.Desc.Geometry.GetVertex(Triangle->Third);
				if (!First || !Second || !Third) break;
				DebugBody.TriangleSample.push_back({*First, *Second, *Third});
			}
			RemainingDebugTriangles -= static_cast<uint32>(DebugBody.TriangleSample.size());
			Result.Bodies.push_back(std::move(DebugBody));
		}
		Result.LastBlockingHit = LastCollisionDebugHit;
		return Result;
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
