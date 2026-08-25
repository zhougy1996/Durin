#pragma once

#include "EngineAPI.h"
#include "DObject/ObjectPtr.h"
#include "Engine/Level.h"
#include "Collision/CollisionTypes.h"
#include "Physics/PhysicsScene.h"

#include "World.gen.h"

namespace Durin
{
	class AActor;
	class APawn;
	class AGameMode;
	class APlayerController;
	class IScene;
	class FGameInputState;

	// Distinguishes editor, play-session, and standalone game world behavior.
	enum class EWorldType : uint8
	{
		Editor,
		Preview,
		PlayInEditor,
		Game
	};

	enum class EWorldPlayState : uint8
	{
		Stopped,
		BeginningPlay,
		Playing,
		EndingPlay
	};

	// Categorizes native gameplay bootstrap failures at the World publication boundary.
	enum class EWorldPlayError : uint8
	{
		None,
		InvalidState,
		MissingLevel,
		CollisionNotReady,
		InvalidGameModeClass,
		GameModeSpawnFailed,
		InvalidPlayerControllerClass,
		InvalidPawnClass,
		MissingPlayerStart,
		PlayerControllerSpawnFailed,
		PawnSpawnFailed,
		PawnPlacementFailed,
		PossessionFailed,
		ViewTargetRejected,
		PlayAborted
	};

	// Requests either lifecycle-only play or one native local-player gameplay session.
	struct FWorldPlayRequest
	{
		DClass* GameModeClass = nullptr;
		AActor* ViewTargetOverride = nullptr;
	};

	struct FWorldPlayResult
	{
		EWorldPlayError Error = EWorldPlayError::None;
		std::string Message;

		explicit operator bool() const { return Error == EWorldPlayError::None; }
	};

	// Makes per-tick timing and the optional Engine-owned raw input source explicit.
	struct FWorldTickContext
	{
		float DeltaSeconds = 0.0f;
		const FGameInputState* GameInput = nullptr;
	};

	enum class EPlayerRestartError : uint8
	{
		None,
		NoGameplaySession,
		ControllerUnavailable,
		GameModeUnavailable,
		InvalidPawnClass,
		MissingPlayerStart,
		PawnSpawnFailed,
		PawnPlacementFailed,
		PossessionFailed,
		ViewTargetRejected,
		RestartAborted
	};

	struct FPlayerRestartRequest
	{
		AActor* ViewTargetOverride = nullptr;
	};

	struct FPlayerRestartResult
	{
		EPlayerRestartError Error = EPlayerRestartError::None;
		std::string Message;
		APawn* Pawn = nullptr;

		explicit operator bool() const { return Error == EPlayerRestartError::None; }
	};

	// Owns the active level and drives actor lifetime, play state, ticking, and physics policy.
	DCLASS(NoClassDefaultObject)
	class DWorld : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DWorld(const FObjectInitializer& ObjectInitializer);
		~DWorld() override = default;
		ENGINE_API auto BeginDestroy() -> void override;
		ENGINE_API auto SpawnActor(DClass* ActorClass, FName InName = FName()) -> AActor*;

		template<typename T>
		auto SpawnActor(FName InName = FName()) -> T*
		{
			return static_cast<T*>(SpawnActor(T::StaticClass(), InName));
		}

		ENGINE_API auto DestroyActor(AActor* Actor) -> bool;
		ENGINE_API auto DestroyAllActors() -> void;
		ENGINE_API auto ContainsActor(const AActor* Actor) const -> bool;
		ENGINE_API auto FindActorByName(FName Name) const -> AActor*;
		ENGINE_API auto GetActors() const -> const std::vector<TObjectPtr<AActor>>&;
		// Immediately changes the active Level while stopped. Gameplay code must request a deferred transition instead.
		ENGINE_API auto SetCurrentLevel(DLevel* Level, bool bDestroyPreviousOwnedLevel = true) -> bool;
		// Queues one Level change for the next World tick so callbacks cannot invalidate their active lifecycle stack.
		ENGINE_API auto RequestLevelTransition(DLevel* Level, bool bDestroyPreviousOwnedLevel = true) -> bool;
		ENGINE_API auto BeginPlay(const FWorldPlayRequest& Request) -> FWorldPlayResult;
		ENGINE_API auto Tick(const FWorldTickContext& Context) -> void;
		ENGINE_API auto EndPlay() -> void;
		ENGINE_API auto RestartPlayer(const FPlayerRestartRequest& Request = {}) -> FPlayerRestartResult;
		ENGINE_API auto SetRenderScene(IScene* InRenderScene) -> void;
		auto HasBegunPlay() const -> bool { return PlayState == EWorldPlayState::BeginningPlay || PlayState == EWorldPlayState::Playing; }
		auto IsEndingPlay() const -> bool { return PlayState == EWorldPlayState::EndingPlay; }
		auto IsPaused() const -> bool { return bPaused; }
		ENGINE_API auto SetPaused(bool bInPaused) -> void;
		auto RequestSingleStep() -> void { bSingleStepRequested = true; }
		auto IsPhysicsSimulationEnabled() const -> bool { return bPhysicsSimulationEnabled; }
		auto SetPhysicsSimulationEnabled(bool bEnabled) -> void { bPhysicsSimulationEnabled = bEnabled; }
		auto GetWorldType() const -> EWorldType { return WorldType; }
		auto SetWorldType(EWorldType InType) -> void { WorldType = InType; }
		auto GetRenderScene() const -> IScene* { return RenderScene; }
		// A world is valid without an active level. Editor and runtime callers must handle nullptr.
		auto GetCurrentLevel() const -> DLevel* { return CurrentLevel.Get(); }
		ENGINE_API auto GetGameMode() const -> AGameMode*;
		ENGINE_API auto GetLocalPlayerController() const -> APlayerController*;
		ENGINE_API auto GetDefaultPawn() const -> APawn*;
		auto GetPhysicsScene() -> FPhysicsScene& { return PhysicsScene; }
		auto GetPhysicsScene() const -> const FPhysicsScene& { return PhysicsScene; }
		auto IsCollisionDebugDrawEnabled() const -> bool { return bCollisionDebugDrawEnabled; }
		ENGINE_API auto SetCollisionDebugDrawEnabled(bool bEnabled) -> void;
		ENGINE_API auto CaptureCollisionDebugSnapshot() const -> FCollisionDebugSnapshot;
		ENGINE_API auto LineTraceSingleByChannel(
			FHitResult& OutHit,
			const FVector3& Start,
			const FVector3& End,
			ECollisionChannel TraceChannel,
			const FCollisionQueryParams& QueryParams = {},
			const FCollisionResponseParams& ResponseParams = {}) const -> bool;
		ENGINE_API auto SweepSingleByChannel(
			FHitResult& OutHit,
			const FCollisionShape& Shape,
			const FTransform& StartTransform,
			const FVector3& Delta,
			ECollisionChannel TraceChannel,
			const FCollisionQueryParams& QueryParams = {},
			const FCollisionResponseParams& ResponseParams = {}) const -> bool;
		ENGINE_API auto OverlapMultiByChannel(
			std::vector<FOverlapResult>& OutOverlaps,
			const FCollisionShape& Shape,
			const FTransform& Transform,
			ECollisionChannel TraceChannel,
			const FCollisionQueryParams& QueryParams = {},
			const FCollisionResponseParams& ResponseParams = {}) const -> bool;

	private:
		struct FNativeGameplaySession
		{
			TObjectPtr<AGameMode> GameMode;
			TObjectPtr<APlayerController> LocalPlayerController;
			TObjectPtr<APawn> DefaultPawn;
			std::vector<TObjectPtr<AActor>> RuntimeActors;
		};

		struct FPendingLevelTransition
		{
			TObjectPtr<DLevel> Level;
			DClass* GameModeClass = nullptr;
			bool bDestroyPreviousOwnedLevel = true;
			bool bResumePlay = false;
		};

		auto OnActorDestroyed(AActor* Actor) -> void;
		auto ClearPendingGameplayIntent() -> void;
		auto ProcessPendingLevelTransition() -> void;
		auto CanContinueTicking(const DLevel* Level) const -> bool;

		// A world may intentionally have no active level during project transitions.
		DPROPERTY(Transient)
		TObjectPtr<DLevel> CurrentLevel;

		// Owned by the world host (DEngine or an editor preview scene). Components
		// retain this endpoint only for the duration of one registration.
		IScene* RenderScene = nullptr;
		EWorldType WorldType = EWorldType::Game;
		EWorldPlayState PlayState = EWorldPlayState::Stopped;
		bool bPaused = false;
		bool bSingleStepRequested = false;
		bool bPhysicsSimulationEnabled = true;
		std::optional<FNativeGameplaySession> GameplaySession;
		std::optional<FPendingLevelTransition> PendingLevelTransition;
		FPhysicsScene PhysicsScene;
		bool bCollisionDebugDrawEnabled = false;
		mutable std::optional<FHitResult> LastCollisionDebugHit;

		friend class DLevel;
		friend class FTickRegistry;
	};
} // namespace Durin
