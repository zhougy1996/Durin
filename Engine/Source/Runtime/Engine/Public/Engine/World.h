#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Engine/Level.h"

#include "World.gen.h"

namespace Durin
{
	class AActor;

	// Distinguishes editor, play-session, and standalone game world behavior.
	enum class EWorldType : uint8
	{
		Editor,
		PlayInEditor,
		Game
	};

	// Owns the active level and drives actor lifetime, play state, ticking, and physics policy.
	DCLASS()
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
		ENGINE_API auto SetCurrentLevel(DLevel* Level, bool bDestroyPreviousOwnedLevel = true) -> bool;
		ENGINE_API auto BeginPlay() -> void;
		ENGINE_API auto Tick(float DeltaSeconds) -> void;
		ENGINE_API auto EndPlay() -> void;
		auto HasBegunPlay() const -> bool { return bHasBegunPlay; }
		auto IsPaused() const -> bool { return bPaused; }
		auto SetPaused(bool bInPaused) -> void { bPaused = bInPaused; }
		auto RequestSingleStep() -> void { bSingleStepRequested = true; }
		auto IsPhysicsSimulationEnabled() const -> bool { return bPhysicsSimulationEnabled; }
		auto SetPhysicsSimulationEnabled(bool bEnabled) -> void { bPhysicsSimulationEnabled = bEnabled; }
		auto GetWorldType() const -> EWorldType { return WorldType; }
		auto SetWorldType(EWorldType InType) -> void { WorldType = InType; }
		// A world is valid without an active level. Editor and runtime callers must handle nullptr.
		auto GetCurrentLevel() const -> DLevel* { return CurrentLevel.Get(); }

	private:
		// A world may intentionally have no active level during project transitions.
		DPROPERTY(Transient)
		TObjectPtr<DLevel> CurrentLevel;

		EWorldType WorldType = EWorldType::Game;
		bool bHasBegunPlay = false;
		bool bPaused = false;
		bool bSingleStepRequested = false;
		bool bPhysicsSimulationEnabled = true;
	};
} // namespace Durin
