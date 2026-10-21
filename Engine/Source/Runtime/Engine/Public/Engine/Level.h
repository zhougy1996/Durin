#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectPtr.h"
#include "DObject/ObjectKey.h"

#include "Asset/AssetReadResult.h"
#include "DObject/AssetPath.h"
#include "EngineAPI.h"
#include "Engine/TickFunction.h"

#include "Level.gen.h"

namespace Durin
{
	class AActor;
	class ACameraActor;
	class DPrimitiveComponent;
	class DSceneComponent;
	class DWorld;

	// Requested names may be uniquified; required names reject collisions before construction.
	DENUM(ScriptType)
	enum class ESpawnActorNameMode : uint8
	{
		Requested,
		RequiredErrorAndReturnNull,
		RequiredReturnNull
	};

	// Owns an actor set, stable actor names, and the level's primary camera selection.
	DCLASS()
	class DLevel : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DLevel(const FObjectInitializer& ObjectInitializer);
		ENGINE_API ~DLevel() override;
		ENGINE_API auto PreEditUndo() -> void override;
		ENGINE_API auto PostEditUndo() -> void override;
		using Super::PostEditUndo;
		auto IsTransactionRestorePending() const -> bool { return PreUndoActors.has_value(); }
		ENGINE_API auto IsActorMembershipChangedDuringUndo(const AActor* Actor) const -> bool;
		ENGINE_API auto SpawnActor(DClass* ActorClass, FName InName = FName(), ESpawnActorNameMode NameMode = ESpawnActorNameMode::Requested) -> AActor*;

		template<typename T>
		auto SpawnActor(FName InName = FName(), ESpawnActorNameMode NameMode = ESpawnActorNameMode::Requested) -> T*
		{
			static_assert(std::is_base_of_v<AActor, T>, "T must derive from AActor");
			return static_cast<T*>(SpawnActor(T::StaticClass(), InName, NameMode));
		}

		ENGINE_API auto DestroyActor(AActor* Actor) -> bool;
		// Reversible editor membership; leaves object/component identity and terminal
		// destruction callbacks intact. Callers restore attachments before registration.
		ENGINE_API auto SetActorPresence(AActor* Actor, bool bPresent,
			size_t ActorIndex = std::numeric_limits<size_t>::max(), bool bRegisterComponents = true) -> bool;
		ENGINE_API auto DestroyAllActors() -> void;
		ENGINE_API auto ContainsActor(const AActor* Actor) const -> bool;
		ENGINE_API auto FindActorByName(FName Name) const -> AActor*;
		ENGINE_API auto RenameActor(AActor* Actor, FName RequestedName, ESpawnActorNameMode NameMode = ESpawnActorNameMode::Requested) -> bool;
		// Includes unpublished preparations and undo-retained objects, not just Actors membership.
		ENGINE_API auto IsActorNameAvailable(FName Name, const AActor* IgnoredActor = nullptr) const -> bool;
		ENGINE_API auto ResolveActorName(FName RequestedName, ESpawnActorNameMode NameMode, const AActor* IgnoredActor = nullptr) const -> FName;
		ENGINE_API auto MakeUniqueActorName(FName RequestedName, const AActor* IgnoredActor = nullptr) const -> FName;
		// Labels are not object identities. Unique editor labels consider published Actors only.
		ENGINE_API auto MakeUniqueActorLabel(const std::string& RequestedLabel, const AActor* IgnoredActor = nullptr,
			std::span<const std::string> ReservedLabels = {}) const -> std::string;
		auto GetActors() const -> const std::vector<TObjectPtr<AActor>>& { return Actors; }

		ENGINE_API auto SetPrimaryCameraActor(ACameraActor* Actor) -> bool;
		auto GetPrimaryCameraActor() const -> ACameraActor* { return PrimaryCameraActor.Get(); }
		auto GetWorld() const -> DWorld* { return OwningWorld; }
		ENGINE_API auto ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError> override;
		ENGINE_API auto PostLoad() -> void override;

#if DURIN_WITH_EDITOR
		auto GetEditorActorHierarchyRevision() const -> uint64 { return EditorActorHierarchyRevision; }
#endif

	private:
		auto SpawnActorInternal(DClass* ActorClass, FName InName, bool bDispatchBeginPlay, ESpawnActorNameMode NameMode = ESpawnActorNameMode::Requested) -> AActor*;
		auto SpawnActorDeferredPlay(DClass* ActorClass, FName InName) -> AActor*;
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
		struct FPreUndoActor
		{
			FObjectKey Actor;
			bool bPresenceChanged = false;
		};
		std::optional<std::vector<FPreUndoActor>> PreUndoActors;
		FTickRegistry TickRegistry;

#if DURIN_WITH_EDITOR
		uint64 EditorActorHierarchyRevision = 1;
#endif

		friend class AActor;
		friend class DSceneComponent;
		friend class DPrimitiveComponent;
		friend class DWorld;
		friend class FTickFunction;
		friend class FTickRegistry;
	};

	// Resolves a package-valued level setting to its unique top-level Level.
	// Package redirects are followed, and failure leaves OutLevelPath unchanged.
	ENGINE_API auto ResolveLevelPackage(
		const FPackagePath& PackagePath,
		FObjectPath& OutLevelPath
	) -> FAssetReadResult;
}
