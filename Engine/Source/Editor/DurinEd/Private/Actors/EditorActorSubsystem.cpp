#include "Actors/EditorActorSubsystem.h"
#include "Editor/EditorCommand.h"
#include "Actors/ActorEditingScope.h"
#include "Actors/ActorAttachmentTransaction.h"
#include "Editor/EditorEngine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "DObject/ObjectLifecycle.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	using namespace Editor;
	DEditorActorSubsystem::DEditorActorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DEditorActorSubsystem::Admit(const FActorEditContext& Context, DLevel* Level, AActor* Owner, bool bDuringEdit) const
		-> std::expected<DTransactor*, FActorEditResult>
	{
		const auto Reject = [](EActorEditError Error, std::string Message) -> std::expected<DTransactor*, FActorEditResult> {
			return std::unexpected(FActorEditResult{{}, std::move(Message), EActorEditStatus::Failed, Error});
		};
		if (!IsInGameThread()) return Reject(EActorEditError::WrongThread, "Actor editing requires the game thread.");
		if (!IsValid(this)) return Reject(EActorEditError::Unavailable, "The actor editing service is unavailable.");
		if (Owner)
		{
			if (!IsValid(Owner)) return Reject(EActorEditError::InvalidTarget, "The owning actor is unavailable.");
			auto* OwnerLevel = Cast<DLevel>(Owner->GetOuter());
			if (Level && Level != OwnerLevel) return Reject(EActorEditError::InvalidTarget, "The actor belongs to another Level.");
			Level = OwnerLevel;
		}
		if (bEditing && !bDuringEdit) return Reject(EActorEditError::HistoryUnavailable, "An actor edit is already in progress.");
		if (Context.bReadOnly) return Reject(EActorEditError::ReadOnly, "The document is read-only.");
		DTransactor* Transactions = TestTransactions;
		if (!Transactions)
		{
			auto* Host = GetEditor();
			const auto Gate = GetWorkGate();
			if (!IsValid(Host) || !Gate || !Gate->IsOpen() || Host->GetEditorSubsystem<DEditorActorSubsystem>() != this)
				return Reject(EActorEditError::Unavailable, "The actor editing service is unavailable or retiring.");
			if (Context.Host != Host) return Reject(EActorEditError::ForeignHost, "The edit belongs to another editor host.");
			if (Host->IsPlaying()) return Reject(EActorEditError::PlayState, "Actor editing is unavailable during play-in-editor.");
			if (!IsValid(Level) || !IsValid(Host->GetEditorWorld()) || Level->GetWorld() != Host->GetEditorWorld()
				|| Host->GetEditorWorld()->GetCurrentLevel() != Level)
				return Reject(EActorEditError::InvalidTarget, "The Level is not owned by this editor host.");
			Transactions = Host->GetTransactor();
		}
		else if (Context.Host) return Reject(EActorEditError::ForeignHost, "An independent operation cannot admit an editor host.");
		if (!IsValid(Level) || Level->IsTemplateObject()
			|| (Owner && (!IsValid(Owner) || Owner->IsTemplateObject() || Owner->IsBeingDestroyed() || Owner->HasBegunPlay()
				|| Owner->GetOuter() != Level || !Level->ContainsActor(Owner))))
			return Reject(EActorEditError::InvalidTarget, "The Level or owning actor is unavailable or detached.");
		if (auto* World = Level->GetWorld(); World && (!IsValid(World) || World->IsShutdownRequested()))
			return Reject(EActorEditError::InvalidTarget, "The owning World is unavailable or shutting down.");
		if (auto* World = Level->GetWorld(); World && World->GetPlayState() != EWorldPlayState::Stopped)
			return Reject(EActorEditError::PlayState, "Actor editing requires a stopped World.");
		if (!IsValid(Transactions) || !Transactions->CanTransact())
			return Reject(EActorEditError::HistoryUnavailable, "Actor editing requires an idle or recording editor transactor.");
		return Transactions;
	}

	namespace
	{
		auto ValidateActors(DLevel& Level, std::span<AActor* const> Actors) -> bool
		{
			std::unordered_set<AActor*> Unique;
			if (Actors.empty()) return false;
			for (auto* Actor : Actors)
				if (!IsValid(Actor) || Actor->IsTemplateObject() || Actor->IsBeingDestroyed() || Actor->HasBegunPlay()
					|| Actor->GetOuter() != &Level || !Level.ContainsActor(Actor) || !Actor->GetRootComponent()
					|| !Unique.insert(Actor).second) return false;
			return true;
		}
		auto SubmitAttachment(DTransactor& Transactions, std::vector<FActorAttachmentTransaction::FEntry> Entries,
			bool bAttach) -> FActorEditResult
		{
			if (Entries.empty()) return {{}, {}, EActorEditStatus::NoOp, EActorEditError::None};
			std::vector<TObjectPtr<AActor>> Actors;
			for (const auto& Entry : Entries) Actors.push_back(Entry.Actor);
			const auto Result = ::Durin::Editor::ApplyEditorChange(Transactions, std::make_unique<FActorAttachmentTransaction>(std::move(Entries), bAttach));
			return Result ? FActorEditResult{std::move(Actors), {}, EActorEditStatus::Changed, EActorEditError::None, Result}
				: FActorEditResult{{}, FormatTransactorResult(Result), EActorEditStatus::Failed, EActorEditError::HistoryRejected, Result};
		}
	}
	auto DEditorActorSubsystem::AttachActors(DLevel& Level, std::span<AActor* const> Actors, AActor* Parent,
		EAttachmentTransformRule Rule, const FActorEditContext& Context) -> FActorEditResult
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return Admission.error();
		return RunActorEdit(GetEditor(), bEditing, [&]() -> FActorEditResult
		{
			if (!ValidateActors(Level, Actors) || !IsValid(Parent) || !ValidateActors(Level, {&Parent, 1}))
				return {{}, "Invalid actor attachment selection."};
			std::vector<FActorAttachmentTransaction::FEntry> Entries;
			for (auto* Actor : Actors)
			{
				if (Parent == Actor) return {{}, "An actor cannot attach to itself."};
				for (auto* Ancestor = Parent; Ancestor; Ancestor = Ancestor->GetAttachParentActor())
					if (Ancestor == Actor) return {{}, "Actor attachment would create a cycle."};
				if (Actor->GetAttachParentActor() != Parent || Rule == EAttachmentTransformRule::SnapToTarget)
					Entries.push_back(MakeActorAttachmentEntry(*Actor, *Parent, Rule));
			}
			return SubmitAttachment(**Admission, std::move(Entries), true);

		});
	}
	auto DEditorActorSubsystem::DetachActors(DLevel& Level, std::span<AActor* const> Actors,
		EDetachmentTransformRule Rule, const FActorEditContext& Context) -> FActorEditResult
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return Admission.error();
		return RunActorEdit(GetEditor(), bEditing, [&]() -> FActorEditResult
		{
			if (!ValidateActors(Level, Actors)) return {{}, "Invalid actor detachment selection."};
			std::vector<FActorAttachmentTransaction::FEntry> Entries;
			for (auto* Actor : Actors)
				if (Actor->GetAttachParentActor()) Entries.push_back(MakeActorDetachmentEntry(*Actor, Rule));
			return SubmitAttachment(**Admission, std::move(Entries), false);

		});
	}
}
