#pragma once

#include "Actors/ActorChange.h"

#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "Transactions/Transactor.h"
#include "Components/SceneComponent.h"
#include "EditorActorSubsystem.gen.h"

namespace Durin { class DLevel; }

namespace Durin::Editor
{
	// Document owners and native adapters must state editability explicitly.
	struct FActorEditContext
	{
		DEditorEngine* Host;
		bool bReadOnly;
		FActorEditContext(DEditorEngine* InHost, bool bInReadOnly) : Host(InHost), bReadOnly(bInReadOnly) {}
	};
	enum class EActorEditStatus : uint8 { Failed, NoOp, Changed };
	enum class EActorEditError : uint8 { None, WrongThread, Unavailable, ReadOnly, ForeignHost, InvalidTarget, PlayState, HistoryUnavailable, InvalidOperation, HistoryRejected };
	struct FActorEditResult
	{
		std::vector<TObjectPtr<AActor>> Actors;
		std::string Message;
		EActorEditStatus Status = EActorEditStatus::Failed;
		EActorEditError Error = EActorEditError::InvalidOperation;
		std::optional<FTransactorResult> History;
		explicit operator bool() const { return Status != EActorEditStatus::Failed; }
		auto Changed() const -> bool { return Status == EActorEditStatus::Changed; }
	};
	struct FComponentEditResult
	{
		DActorComponent* Component = nullptr;
		std::string Message;
		EActorEditStatus Status = EActorEditStatus::Failed;
		EActorEditError Error = EActorEditError::InvalidOperation;
		std::optional<FTransactorResult> History;
		explicit operator bool() const { return Status != EActorEditStatus::Failed; }
		auto Changed() const -> bool { return Status == EActorEditStatus::Changed; }
	};
	// Native compound owners supply prepared, unpublished actors. The returned change
	// retains identities for replay; the preparer owns cleanup until publication.
	DURINED_API auto MakeActorPresenceChange(DLevel& Level, std::span<AActor* const> Actors,
		bool bRemove, std::string Description) -> std::unique_ptr<FActorCommandChange>;
}

namespace Durin
{
	struct FEditorActorSubsystemTestAccess;
	// General actor/component edits share the admitting editor host's history.
	DCLASS(NoClassDefaultObject)
	class DEditorActorSubsystem : public DEditorSubsystem
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DEditorActorSubsystem(const FObjectInitializer& Initializer);
		DURINED_API auto SpawnActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
			const Editor::FActorEditContext& Context, std::function<void(AActor&)> Configure = {}) -> Editor::FActorEditResult;
		DURINED_API auto DestroyActors(DLevel& Level, std::span<AActor* const> Actors, const Editor::FActorEditContext& Context) -> Editor::FActorEditResult;
		DURINED_API auto DuplicateActors(DLevel& Level, std::span<AActor* const> Actors, const Editor::FActorEditContext& Context) -> Editor::FActorEditResult;
		DURINED_API auto RenameActor(DLevel& Level, AActor* Actor, FName Name, const Editor::FActorEditContext& Context) -> Editor::FActorEditResult;
		DURINED_API auto AttachActors(DLevel& Level, std::span<AActor* const> Actors, AActor* Parent,
			EAttachmentTransformRule Rule, const Editor::FActorEditContext& Context) -> Editor::FActorEditResult;
		DURINED_API auto DetachActors(DLevel& Level, std::span<AActor* const> Actors,
			EDetachmentTransformRule Rule, const Editor::FActorEditContext& Context) -> Editor::FActorEditResult;
		DURINED_API auto AddComponent(AActor& Owner, DClass* Class, DSceneComponent* Parent, const Editor::FActorEditContext& Context) -> Editor::FComponentEditResult;
		DURINED_API auto DuplicateComponent(AActor& Owner, DActorComponent* Source, const Editor::FActorEditContext& Context) -> Editor::FComponentEditResult;
		DURINED_API auto DeleteComponent(AActor& Owner, DActorComponent* Component, const Editor::FActorEditContext& Context) -> Editor::FComponentEditResult;
		DURINED_API auto RenameComponent(AActor& Owner, DActorComponent* Component, FName Name, const Editor::FActorEditContext& Context) -> Editor::FComponentEditResult;
		DURINED_API auto ReparentComponent(AActor& Owner, DSceneComponent* Component, DSceneComponent* Parent, const Editor::FActorEditContext& Context) -> Editor::FComponentEditResult;
	private:
		auto Admit(const Editor::FActorEditContext& Context, DLevel* Level, AActor* Owner = nullptr,
			bool bDuringEdit = false) const -> std::expected<DTransactor*, Editor::FActorEditResult>;
		DTransactor* TestTransactions = nullptr;
		bool bEditing = false;
		friend struct FEditorActorSubsystemTestAccess;
	};
}
