#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Engine/TickFunction.h"
#include "IScene.h"
#include "Math/Box.h"

#include "Level.gen.h"

namespace Durin
{
	class AActor;
	class ACameraActor;
	class DPrimitiveComponent;
	class DSceneComponent;
	class DWorld;

#if DURIN_WITH_EDITOR
	enum class EEditorPickingPrimitiveFamily : uint8
	{
		Unsupported,
		StaticMesh,
		SkeletalMesh,
		Terrain
	};

	// Describes one game-thread primitive mutation without retaining reflected objects.
	struct FEditorPickingPrimitiveMutation
	{
		TWeakObjectPtr<AActor> Actor;
		TWeakObjectPtr<DPrimitiveComponent> Component;
		FPrimitiveSceneId PrimitiveId = InvalidPrimitiveSceneId;
		uint64 RegistrationGeneration = 0;
		EEditorPickingPrimitiveFamily Family = EEditorPickingPrimitiveFamily::Unsupported;
		FBox WorldBounds;
		bool bVisible = false;
		bool bRetired = false;
	};

	// Carries either one ordered mutation or one complete initial/recovery snapshot.
	struct FEditorPickingPrimitiveMutationBatch
	{
		uint64 Revision = 0;
		bool bCompleteSnapshot = false;
		std::vector<FEditorPickingPrimitiveMutation> Mutations;
	};
#endif

	// Owns an actor set, stable actor names, and the level's primary camera selection.
	DCLASS()
	class DLevel : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DLevel(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DLevel() override;
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
		using FEditorPickingPrimitiveObserver = std::function<void(const FEditorPickingPrimitiveMutationBatch&)>;
		// Registers a game-thread observer and synchronously supplies one complete snapshot.
		ENGINE_API auto SubscribeEditorPickingPrimitives(FEditorPickingPrimitiveObserver Observer) -> uint64;
		ENGINE_API auto UnsubscribeEditorPickingPrimitives(uint64 Subscription) -> void;
		ENGINE_API auto CaptureEditorPickingPrimitiveSnapshot() const -> FEditorPickingPrimitiveMutationBatch;
#endif

	private:
		auto SpawnActorInternal(DClass* ActorClass, FName InName, bool bDispatchBeginPlay) -> AActor*;
		auto SpawnActorDeferredPlay(DClass* ActorClass, FName InName) -> AActor*;
		auto MakeUniqueActorName(FName RequestedName, const AActor* IgnoredActor = nullptr) const -> FName;
		auto OnActorAdded(AActor* Actor) -> void;
		auto SetOwningWorld(DWorld* World) -> void { OwningWorld = World; }

#if DURIN_WITH_EDITOR
		auto NotifyEditorActorHierarchyChanged() -> void { ++EditorActorHierarchyRevision; }
		auto NotifyEditorPickingPrimitiveChanged(DPrimitiveComponent* Component, bool bRetired = false) -> void;
#endif

		DPROPERTY()
		std::vector<TObjectPtr<AActor>> Actors;

		DPROPERTY()
		TObjectPtr<ACameraActor> PrimaryCameraActor;

		DWorld* OwningWorld = nullptr;
		FTickRegistry TickRegistry;

#if DURIN_WITH_EDITOR
		uint64 EditorActorHierarchyRevision = 1;
		uint64 EditorPickingPrimitiveRevision = 1;
		uint64 NextEditorPickingObserverId = 1;
		bool bDispatchingEditorPickingMutation = false;
		std::unordered_map<uint64, FEditorPickingPrimitiveObserver> EditorPickingPrimitiveObservers;
#endif

		friend class AActor;
		friend class DSceneComponent;
		friend class DPrimitiveComponent;
		friend class DWorld;
		friend class FTickFunction;
		friend class FTickRegistry;
	};
}
