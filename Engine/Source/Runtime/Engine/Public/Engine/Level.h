#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "Level.gen.h"

namespace Durin
{
	class AActor;
	class ACameraActor;
	class DSceneComponent;
	class DWorld;

	// Owns an actor set, stable actor names, and the level's primary camera selection.
	DCLASS()
	class DLevel : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DLevel(const FObjectInitializer& ObjectInitializer);
		~DLevel() override = default;
		ENGINE_API auto SpawnActor(DClass* ActorClass, FName InName = FName()) -> AActor*;

		template<typename T>
		auto SpawnActor(FName InName = FName()) -> T*
		{
			static_assert(std::is_base_of_v<AActor, T>, "T must derive from AActor");
			return static_cast<T*>(SpawnActor(T::StaticClass(), InName));
		}

		ENGINE_API auto DestroyActor(AActor* Actor) -> bool;
		ENGINE_API auto DestroyAllActors() -> void;
		ENGINE_API auto ContainsActor(const AActor* Actor) const -> bool;
		ENGINE_API auto FindActorByName(FName Name) const -> AActor*;
		ENGINE_API auto RenameActor(AActor* Actor, FName RequestedName) -> bool;
		auto GetActors() const -> const std::vector<TObjectPtr<AActor>>& { return Actors; }

		ENGINE_API auto SetPrimaryCameraActor(ACameraActor* Actor) -> bool;
		auto GetPrimaryCameraActor() const -> ACameraActor* { return PrimaryCameraActor.Get(); }
		auto GetWorld() const -> DWorld* { return OwningWorld; }
		ENGINE_API auto PostLoad(std::string& OutError) -> bool override;

#if DURIN_WITH_EDITOR
		auto GetEditorActorHierarchyRevision() const -> uint64 { return EditorActorHierarchyRevision; }
#endif

	private:
		auto MakeUniqueActorName(FName RequestedName, const AActor* IgnoredActor = nullptr) const -> FName;
		auto OnActorAdded(AActor* Actor) -> void;
		auto SetOwningWorld(DWorld* World) -> void { OwningWorld = World; }

#if DURIN_WITH_EDITOR
		auto NotifyEditorActorHierarchyChanged() -> void { ++EditorActorHierarchyRevision; }
#endif

		DPROPERTY()
		std::vector<TObjectPtr<AActor>> Actors;

		DPROPERTY()
		TObjectPtr<ACameraActor> PrimaryCameraActor;

		DWorld* OwningWorld = nullptr;

#if DURIN_WITH_EDITOR
		uint64 EditorActorHierarchyRevision = 1;
#endif

		friend class AActor;
		friend class DSceneComponent;
		friend class DWorld;
	};
}
