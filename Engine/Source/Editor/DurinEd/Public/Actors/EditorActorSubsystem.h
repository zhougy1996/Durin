#pragma once

#include "CoreMinimal.h"
#include "Actors/ActorChange.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/Class.h"
#include "Transactions/Transactor.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "EditorActorSubsystem.gen.h"

namespace Durin { class DClass; }

namespace Durin::Editor
{
	// Document owners and native adapters must state editability explicitly.
	struct FActorEditContext
	{
		bool bReadOnly;
		FActorEditContext(bool bInReadOnly) : bReadOnly(bInReadOnly) {}
	};
	// Native compound owners supply prepared, unpublished actors. The returned change
	// retains identities for replay; the preparer owns cleanup until publication.
	DURINED_API auto MakeActorPresenceChange(DLevel& Level, std::span<AActor* const> Actors,
		bool bRemove, std::string Description) -> std::unique_ptr<FActorCommandChange>;
}

namespace Durin
{
	// One synchronous Level snapshot; children retain Level order and values are copies.
	DSTRUCT(ScriptType)
	struct FActorScriptInfo
	{
		GENERATED_BODY()
		DPROPERTY(ScriptReadOnly)
		TObjectPtr<AActor> Actor;
		DPROPERTY(ScriptReadOnly)
		std::string Name;
		DPROPERTY(ScriptReadOnly)
		DClass* ActorClass = nullptr;
		DPROPERTY(ScriptReadOnly)
		TObjectPtr<AActor> Parent;
		DPROPERTY(ScriptReadOnly)
		std::vector<TObjectPtr<AActor>> Children;
		DPROPERTY(ScriptReadOnly)
		TObjectPtr<DSceneComponent> RootComponent;
		DPROPERTY(ScriptReadOnly)
		FTransform WorldTransform;
	};
	// General actor/component edits share the admitting editor host's history.
	DCLASS(ScriptType, NoClassDefaultObject)
	class DEditorActorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorActorSubsystem(const FObjectInitializer& Initializer);
		// Script queries require this Host's active job and inspect only its current editor Level.
		DFUNCTION(ScriptCallable, ScriptName = "get_editor_level")
		DURINED_API auto ScriptGetEditorLevel() const -> DLevel*;
		DFUNCTION(ScriptCallable, ScriptName = "get_all_level_actors")
		DURINED_API auto ScriptGetAllLevelActors(DLevel* Level) const -> std::vector<TObjectPtr<AActor>>;
		DFUNCTION(ScriptCallable, ScriptName = "get_actor_info")
		DURINED_API auto ScriptGetActorInfo(DLevel* Level, AActor* Actor) const -> FActorScriptInfo;
		// Script edits return native payloads; domain rejection returns None, an empty array or false.
		DFUNCTION(ScriptCallable, ScriptName = "spawn_actor")
		DURINED_API auto ScriptSpawnActor(DLevel* Level, DClass* ActorClass, const FTransform& Transform, const std::string& Name = "") -> AActor*;
		DFUNCTION(ScriptCallable, ScriptName = "destroy_actors")
		DURINED_API auto ScriptDestroyActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors) -> bool;
		DFUNCTION(ScriptCallable, ScriptName = "duplicate_actors")
		DURINED_API auto ScriptDuplicateActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors) -> std::vector<TObjectPtr<AActor>>;
		DFUNCTION(ScriptCallable, ScriptName = "rename_actor")
		DURINED_API auto ScriptRenameActor(DLevel* Level, AActor* Actor, const std::string& Name) -> bool;
		DFUNCTION(ScriptCallable, ScriptName = "attach_actors")
		DURINED_API auto ScriptAttachActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors, AActor* Parent, EAttachmentTransformRule Rule = EAttachmentTransformRule::KeepWorld) -> bool;
		DFUNCTION(ScriptCallable, ScriptName = "detach_actors")
		DURINED_API auto ScriptDetachActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors, EDetachmentTransformRule Rule = EDetachmentTransformRule::KeepWorld) -> bool;
		// Failed creation returns null, failed duplication an empty array, and other failures false.
		// Successful no-ops return true without new history. Detailed recovery state belongs to the transactor.
		DURINED_API auto SpawnActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
			const Editor::FActorEditContext& Context, std::function<void(AActor&)> Configure = {}) -> AActor*;
		DURINED_API auto DestroyActors(DLevel& Level, std::span<AActor* const> Actors, const Editor::FActorEditContext& Context) -> bool;
		DURINED_API auto DuplicateActors(DLevel& Level, std::span<AActor* const> Actors, const Editor::FActorEditContext& Context) -> std::vector<TObjectPtr<AActor>>;
		DURINED_API auto RenameActor(DLevel& Level, AActor* Actor, FName Name, const Editor::FActorEditContext& Context) -> bool;
		DURINED_API auto AttachActors(DLevel& Level, std::span<AActor* const> Actors, AActor* Parent,
			EAttachmentTransformRule Rule, const Editor::FActorEditContext& Context) -> bool;
		DURINED_API auto DetachActors(DLevel& Level, std::span<AActor* const> Actors,
			EDetachmentTransformRule Rule, const Editor::FActorEditContext& Context) -> bool;
		DURINED_API auto AddComponent(AActor& Owner, DClass* Class, DSceneComponent* Parent, const Editor::FActorEditContext& Context) -> DActorComponent*;
		DURINED_API auto DuplicateComponent(AActor& Owner, DActorComponent* Source, const Editor::FActorEditContext& Context) -> DActorComponent*;
		DURINED_API auto DeleteComponent(AActor& Owner, DActorComponent* Component, const Editor::FActorEditContext& Context) -> bool;
		DURINED_API auto RenameComponent(AActor& Owner, DActorComponent* Component, FName Name, const Editor::FActorEditContext& Context) -> bool;
		DURINED_API auto ReparentComponent(AActor& Owner, DSceneComponent* Component, DSceneComponent* Parent, const Editor::FActorEditContext& Context) -> bool;
	private:
		auto ScriptEditContext(DLevel* Level) const -> std::optional<Editor::FActorEditContext>;
		auto Admit(const Editor::FActorEditContext& Context, DLevel* Level, AActor* Owner = nullptr,
			bool bDuringEdit = false) const -> DTransactor*;
		bool bEditing = false;
	};
}
