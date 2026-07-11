#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Engine/Level.h"

#include "World.gen.h"

namespace Durin
{
	class AActor;

	DCLASS()
	class ENGINE_API DWorld : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DWorld(const FObjectInitializer& ObjectInitializer);
		~DWorld() override = default;
		auto BeginDestroy() -> void override;
		auto SpawnActor(DClass* ActorClass, FName InName = FName()) -> AActor*;

		template<typename T>
		auto SpawnActor(FName InName = FName()) -> T*
		{
			return static_cast<T*>(SpawnActor(T::StaticClass(), InName));
		}

		auto DestroyActor(AActor* Actor) -> bool;
		auto DestroyAllActors() -> void;
		auto ContainsActor(const AActor* Actor) const -> bool;
		auto FindActorByName(FName Name) const -> AActor*;
		auto GetActors() const -> const std::vector<TObjectPtr<AActor>>&;
		auto SetCurrentLevel(DLevel* Level) -> bool;
		// A world is valid without an active level. Editor and runtime callers must handle nullptr.
		auto GetCurrentLevel() const -> DLevel* { return CurrentLevel.Get(); }

	private:
		DPROPERTY(Transient)
		TObjectPtr<DLevel> CurrentLevel;
	};
} // namespace Durin
