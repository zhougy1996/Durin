#include "Actors/EditorActorSubsystem.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorEditingScope.h"
#include "Components/SceneComponent.h"
#include "Editor/EditorEngine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "DObject/ObjectLifecycle.h"
#include "Threading/RunnableThread.h"
#include "Tools/EditorScriptCall.h"

namespace Durin
{
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
		if (!IsValid(Host) || Host != GEditor || Host->IsShutdownRequested()
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
		struct FAttachmentEdit
		{
			DSceneComponent* Root;
			DSceneComponent* OldParent;
			FTransform BeforeRelative;
			FTransform AfterRelative;
		};

		auto SetAttachment(const FAttachmentEdit& Edit, DSceneComponent* Parent, const FTransform& Relative) -> bool
		{
			if (Edit.Root->GetAttachParent() != Parent)
			{
				const bool Changed = Parent ? Edit.Root->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative)
					: Edit.Root->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
				if (!Changed) return false;
			}
			Edit.Root->SetRelativeTransform(Relative);
			return true;
		}

		auto SubmitAttachment(DTransactor& Transactions, const std::vector<FAttachmentEdit>& Edits,
			DSceneComponent* Parent) -> bool
		{
			if (Edits.empty()) return true;
			FScopedTransaction Scope(&Transactions, {"ActorAttachment", Parent ? "Attach actors" : "Detach actors"});
			if (!Scope.IsOutstanding()) return false;
			for (const auto& Edit : Edits)
			{
				Edit.Root->SetFlags(EObjectFlags::Transactional);
				if (!Edit.Root->Modify(false)) return false;
			}
			for (size_t Index = 0; Index < Edits.size(); ++Index)
			{
				if (SetAttachment(Edits[Index], Parent, Edits[Index].AfterRelative)) continue;
				while (Index > 0)
				{
					const auto& Previous = Edits[--Index];
					SetAttachment(Previous, Previous.OldParent, Previous.BeforeRelative);
				}
				return false;
			}
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
			std::vector<FAttachmentEdit> Entries;
			for (auto* Actor : Actors)
			{
				if (Parent == Actor) return false;
				for (auto* Ancestor = Parent; Ancestor; Ancestor = Ancestor->GetAttachParentActor())
					if (Ancestor == Actor) return false;
				if (Actor->GetAttachParentActor() != Parent || Rule == EAttachmentTransformRule::SnapToTarget)
				{
					auto* Root = Actor->GetRootComponent();
					if (Parent->GetRootComponent()->IsAttachedTo(Root)) return false;
					FTransform Relative = Root->GetRelativeTransform();
					if (Rule == EAttachmentTransformRule::KeepWorld)
						Relative = FTransform::MakeRelative(Root->GetWorldTransform(), Parent->GetActorTransform());
					else if (Rule == EAttachmentTransformRule::SnapToTarget) Relative = FTransform{};
					Entries.push_back({Root, Root->GetAttachParent(), Root->GetRelativeTransform(), Relative});
				}
			}
			return SubmitAttachment(*Admission, Entries, Parent->GetRootComponent());

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
			std::vector<FAttachmentEdit> Entries;
			for (auto* Actor : Actors)
				if (Actor->GetAttachParentActor()) {
					auto* Root = Actor->GetRootComponent();
					Entries.push_back({Root, Root->GetAttachParent(), Root->GetRelativeTransform(),
						Rule == EDetachmentTransformRule::KeepWorld ? Root->GetWorldTransform() : Root->GetRelativeTransform()});
				}
			return SubmitAttachment(*Admission, Entries, nullptr);

		});
	}
}
