#pragma once

#include "CoreMinimal.h"
#include "Actors/ActorChange.h"
#include "Actors/ActorEditContext.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/Class.h"
#include "Transactions/Transactor.h"
#include "Components/SceneComponent.h"
#include "Engine/Level.h"
#include "EditorActorSubsystem.gen.h"

namespace Durin { class DClass; }

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
		std::string Label;
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
	DCLASS(ScriptType)
	class DEditorActorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorActorSubsystem(const FObjectInitializer& Initializer);
		// Script queries require this Host's active job and inspect only its current editor Level.
		DFUNCTION(ScriptCallable, ScriptName = "GetEditorLevel")
		DURINED_API auto ScriptGetEditorLevel() const -> DLevel*;
		DFUNCTION(ScriptCallable, ScriptName = "GetAllLevelActors")
		DURINED_API auto ScriptGetAllLevelActors(DLevel* Level) const -> std::vector<TObjectPtr<AActor>>;
		DFUNCTION(ScriptCallable, ScriptName = "GetActorInfo")
		DURINED_API auto ScriptGetActorInfo(DLevel* Level, AActor* Actor) const -> FActorScriptInfo;
		DFUNCTION(ScriptCallable, ScriptName = "GetComponents")
		DURINED_API auto ScriptGetComponents(DLevel* Level, AActor* Actor, DClass* ComponentClass = nullptr) const
			-> std::vector<TObjectPtr<DActorComponent>>;
		// Script edits return native payloads; domain rejection returns None, an empty array or false.
		DFUNCTION(ScriptCallable, ScriptName = "SpawnActor")
		DURINED_API auto ScriptSpawnActor(DLevel* Level, DClass* ActorClass, const FTransform& Transform, const std::string& Name = "",
			const std::string& Label = "", ESpawnActorNameMode NameMode = ESpawnActorNameMode::Requested) -> AActor*;
		DFUNCTION(ScriptCallable, ScriptName = "DestroyActors")
		DURINED_API auto ScriptDestroyActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors) -> bool;
		DFUNCTION(ScriptCallable, ScriptName = "DuplicateActors")
		DURINED_API auto ScriptDuplicateActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors) -> std::vector<TObjectPtr<AActor>>;
		DFUNCTION(ScriptCallable, ScriptName = "RenameActor")
		DURINED_API auto ScriptRenameActor(DLevel* Level, AActor* Actor, const std::string& Name,
			ESpawnActorNameMode NameMode = ESpawnActorNameMode::Requested) -> bool;
		DFUNCTION(ScriptCallable, ScriptName = "SetActorLabel")
		DURINED_API auto ScriptSetActorLabel(DLevel* Level, AActor* Actor, const std::string& Label, bool MakeUnique = true) -> bool;
		DFUNCTION(ScriptCallable, ScriptName = "AttachActors")
		DURINED_API auto ScriptAttachActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors, AActor* Parent, EAttachmentTransformRule Rule = EAttachmentTransformRule::KeepWorld) -> bool;
		DFUNCTION(ScriptCallable, ScriptName = "DetachActors")
		DURINED_API auto ScriptDetachActors(DLevel* Level, const std::vector<TObjectPtr<AActor>>& Actors, EDetachmentTransformRule Rule = EDetachmentTransformRule::KeepWorld) -> bool;
		// Failed creation returns null, failed duplication an empty array, and other failures false.
		// Attachment and detachment report whether any root changed. Other successful no-ops return true. The transactor reports history failures.
		DURINED_API auto SpawnActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
			const FActorEditContext& Context, std::function<void(AActor&)> Configure = {},
			ESpawnActorNameMode NameMode = ESpawnActorNameMode::Requested, const std::string& Label = "", std::string* OutError = nullptr) -> AActor*;
		// Checks the host's edit policy for an existing level actor.
		DURINED_API auto CanEditActor(AActor& Actor, const FActorEditContext& Context) const -> std::expected<void, std::string>;
		DURINED_API auto DestroyActors(DLevel& Level, std::span<AActor* const> Actors, const FActorEditContext& Context) -> bool;
		DURINED_API auto DuplicateActors(DLevel& Level, std::span<AActor* const> Actors, const FActorEditContext& Context) -> std::vector<TObjectPtr<AActor>>;
		DURINED_API auto RenameActor(DLevel& Level, AActor* Actor, FName Name, const FActorEditContext& Context,
			ESpawnActorNameMode NameMode = ESpawnActorNameMode::Requested) -> bool;
		DURINED_API auto SetActorLabel(DLevel& Level, AActor* Actor, const std::string& Label,
			const FActorEditContext& Context, bool bMakeUnique = true) -> bool;
		DURINED_API auto AttachActors(DLevel& Level, std::span<AActor* const> Actors, AActor* Parent,
			EAttachmentTransformRule Rule, const FActorEditContext& Context) -> bool;
		DURINED_API auto DetachActors(DLevel& Level, std::span<AActor* const> Actors,
			EDetachmentTransformRule Rule, const FActorEditContext& Context) -> bool;
		DURINED_API auto AddComponent(AActor& Owner, DClass* Class, DSceneComponent* Parent, const FActorEditContext& Context) -> DActorComponent*;
		DURINED_API auto DuplicateComponent(AActor& Owner, DActorComponent* Source, const FActorEditContext& Context) -> DActorComponent*;
		DURINED_API auto DeleteComponent(AActor& Owner, DActorComponent* Component, const FActorEditContext& Context) -> bool;
		DURINED_API auto RenameComponent(AActor& Owner, DActorComponent* Component, FName Name, const FActorEditContext& Context) -> bool;
		DURINED_API auto ReparentComponent(AActor& Owner, DSceneComponent* Component, DSceneComponent* Parent, const FActorEditContext& Context) -> bool;
	private:
		friend class FActorFactory;
		auto SpawnConfiguredActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
			const FActorEditContext& Context, const std::function<void(AActor&)>& Configure, ESpawnActorNameMode NameMode,
			const std::string& Label, std::string* OutError) -> AActor*;
		auto ScriptEditContext(DLevel* Level) const -> std::optional<FActorEditContext>;
		auto ValidateEditTarget(const FActorEditContext& Context, DLevel* Level, AActor* Owner, std::string* OutError = nullptr) const -> bool;
		template<typename F>
		auto RunActorEdit(const FActorEditContext& Context, DLevel* Level, AActor* Owner, F Operation,
			std::string* OutError = nullptr) -> std::invoke_result_t<F>;
		bool bEditing = false;
	};
}
