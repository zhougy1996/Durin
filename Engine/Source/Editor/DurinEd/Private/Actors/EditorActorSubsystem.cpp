#include "Actors/EditorActorSubsystem.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorEditingScope.h"
#include "Actors/ActorAttachmentTransaction.h"
#include "Editor/EditorEngine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "DObject/ObjectLifecycle.h"
#include "Threading/RunnableThread.h"
#include "Tools/EditorScriptCall.h"

namespace Durin
{
	using namespace Editor;
	DEditorActorSubsystem::DEditorActorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DEditorActorSubsystem::Admit(const FActorEditContext& Context, DLevel* Level, AActor* Owner, bool bDuringEdit) const
		-> DTransactor*
	{

		if (!IsInGameThread()) return nullptr;
		if (!IsValid(this)) return nullptr;
		if (Owner)
		{
			if (!IsValid(Owner)) return nullptr;
			auto* OwnerLevel = Cast<DLevel>(Owner->GetOuter());
			if (Level && Level != OwnerLevel) return nullptr;
			Level = OwnerLevel;
		}
		if (bEditing && !bDuringEdit) return nullptr;
		if (Context.bReadOnly) return nullptr;
		auto* Host = GetEditor();
		const auto Gate = GetWorkGate();
		if (!IsValid(Host) || Host != GEditor || !Gate || !Gate->IsOpen()
			|| Host->GetEditorSubsystem<DEditorActorSubsystem>() != this)
			return nullptr;
		if (Host->IsPlaying()) return nullptr;
		if (!IsValid(Level) || !IsValid(Host->GetEditorWorld()) || Level->GetWorld() != Host->GetEditorWorld()
			|| Host->GetEditorWorld()->GetCurrentLevel() != Level)
			return nullptr;
		auto* Transactions = Host->GetTransactor();
		if (!IsValid(Level) || Level->IsTemplateObject()
			|| (Owner && (!IsValid(Owner) || Owner->IsTemplateObject() || Owner->IsBeingDestroyed() || Owner->HasBegunPlay()
				|| Owner->GetOuter() != Level || !Level->ContainsActor(Owner))))
			return nullptr;
		if (auto* World = Level->GetWorld(); World && (!IsValid(World) || World->IsShutdownRequested()))
			return nullptr;
		if (auto* World = Level->GetWorld(); World && World->GetPlayState() != EWorldPlayState::Stopped)
			return nullptr;
		if (auto* Script = FScopedEditorScriptCall::Current())
		{
			if (!Script->IsAdmitted() || Script->GetContext().Host != Host)
				return nullptr;
			const auto Access = Script->ResolveEditAccess(*Level);
			if (Access != EEditorScriptTargetAccess::Writable)
				return nullptr;
		}
		if (!IsValid(Transactions) || !Transactions->CanTransact())
			return nullptr;
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
			bool bAttach) -> bool
		{
			if (Entries.empty()) return true;
			const auto Result = ::Durin::Editor::ApplyEditorChange(Transactions, std::make_unique<FActorAttachmentTransaction>(std::move(Entries), bAttach));
			if (!Result) return false;
			return true;
		}
	}
	auto DEditorActorSubsystem::AttachActors(DLevel& Level, std::span<AActor* const> Actors, AActor* Parent,
		EAttachmentTransformRule Rule, const FActorEditContext& Context) -> bool
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			if (!ValidateActors(Level, Actors) || !IsValid(Parent) || !ValidateActors(Level, {&Parent, 1}))
				return false;
			std::vector<FActorAttachmentTransaction::FEntry> Entries;
			for (auto* Actor : Actors)
			{
				if (Parent == Actor) return false;
				for (auto* Ancestor = Parent; Ancestor; Ancestor = Ancestor->GetAttachParentActor())
					if (Ancestor == Actor) return false;
				if (Actor->GetAttachParentActor() != Parent || Rule == EAttachmentTransformRule::SnapToTarget)
					Entries.push_back(MakeActorAttachmentEntry(*Actor, *Parent, Rule));
			}
			return SubmitAttachment(*Admission, std::move(Entries), true);

		});
	}
	auto DEditorActorSubsystem::DetachActors(DLevel& Level, std::span<AActor* const> Actors,
		EDetachmentTransformRule Rule, const FActorEditContext& Context) -> bool
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			if (!ValidateActors(Level, Actors)) return false;
			std::vector<FActorAttachmentTransaction::FEntry> Entries;
			for (auto* Actor : Actors)
				if (Actor->GetAttachParentActor()) Entries.push_back(MakeActorDetachmentEntry(*Actor, Rule));
			return SubmitAttachment(*Admission, std::move(Entries), false);

		});
	}
}
