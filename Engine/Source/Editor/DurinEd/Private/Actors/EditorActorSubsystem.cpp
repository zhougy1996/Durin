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

	auto DEditorActorSubsystem::CanEditActor(AActor& Actor, const FActorEditContext& Context) const -> std::expected<void, std::string>
	{
		std::string Error;
		if (!Admit(Context, nullptr, &Actor, false, &Error)) return std::unexpected(std::move(Error));
		return {};
	}

	auto DEditorActorSubsystem::Admit(const FActorEditContext& Context, DLevel* Level, AActor* Owner, bool bDuringEdit, std::string* OutError) const
		-> DTransactor*
	{

		auto Reject = [&](const char* Message) -> DTransactor* {
			if (OutError) *OutError = Message;
			return nullptr;
		};
		if (!IsInGameThread()) return Reject("Actor edits require the game thread.");
		if (!IsValid(this)) return Reject("The actor subsystem is unavailable.");
		if (Owner)
		{
			if (!IsValid(Owner)) return Reject("The actor is unavailable.");
			auto* OwnerLevel = Cast<DLevel>(Owner->GetOuter());
			if (Level && Level != OwnerLevel) return Reject("The actor belongs to another level.");
			Level = OwnerLevel;
		}
		if (bEditing && !bDuringEdit) return Reject("Another actor edit is in progress.");
		if (Context.bReadOnly) return Reject("The level is read-only.");
		auto* Host = GetEditor();
		if (!IsValid(Host) || Host != GEditor || Host->IsShutdownRequested()
			|| Host->GetEditorSubsystem<DEditorActorSubsystem>() != this)
			return Reject("The editor host is unavailable or shutting down.");
		if (Host->IsPlaying()) return Reject("Actor editing is unavailable during play.");
		if (!IsValid(Level) || !IsValid(Host->GetEditorWorld()) || Level->GetWorld() != Host->GetEditorWorld()
			|| Host->GetEditorWorld()->GetCurrentLevel() != Level)
			return Reject("The target is not the current editor level.");
		auto* Transactions = Host->GetTransactor();
		if (!IsValid(Level) || Level->IsTemplateObject()
			|| (Owner && (!IsValid(Owner) || Owner->IsTemplateObject() || Owner->IsBeingDestroyed() || Owner->HasBegunPlay()
				|| Owner->GetOuter() != Level || !Level->ContainsActor(Owner))))
			return Reject("The level or actor is unavailable for editing.");
		if (auto* World = Level->GetWorld(); World && (!IsValid(World) || World->IsShutdownRequested()))
			return Reject("The world is shutting down.");
		if (auto* World = Level->GetWorld(); World && World->GetPlayState() != EWorldPlayState::Stopped)
			return Reject("The world is playing.");
		if (auto* Script = FScopedEditorScriptCall::Current())
		{
			if (!Script->IsAdmitted() || Script->GetContext().Host != Host)
				return Reject("The script is not admitted by this editor.");
			const auto Access = Script->ResolveEditAccess(*Level);
			if (Access != EEditorScriptTargetAccess::Writable)
				return Reject("The script cannot write to this level.");
		}
		if (!IsValid(Transactions) || !Transactions->CanTransact())
			return Reject("Editor history is unavailable for recording.");
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
		auto WouldCreateCycle(AActor& Child, AActor& Parent) -> bool
		{
			for (auto* Ancestor = &Parent; Ancestor; Ancestor = Ancestor->GetAttachParentActor())
				if (Ancestor == &Child) return true;
			return Parent.GetRootComponent()->IsAttachedTo(Child.GetRootComponent());
		}

		template<class Edit>
		auto SubmitAttachment(DLevel& Level, std::span<AActor* const> Actors,
			const char* Description, Edit Apply) -> bool
		{
			// Notifications may retire a later actor before its turn.
			std::vector<TObjectPtr<AActor>> Targets(Actors.begin(), Actors.end());
			FScopedTransaction Scope("ActorAttachment", Description, nullptr);
			if (!Scope.IsOutstanding()) return false;
			bool Changed = false;
			for (const auto& Target : Targets)
			{
				auto* Actor = Target.Get();
				if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsBeingDestroyed()) continue;
				auto* Root = Actor->GetRootComponent();
				if (!IsValid(Root) || !Apply(*Root)) continue;
				Changed = true;
				if (Target) Target->MarkPackageDirty();
			}
			return Changed;
		}

	}
	auto DEditorActorSubsystem::AttachActors(DLevel& Level, std::span<AActor* const> Actors, AActor* Parent,
		EAttachmentTransformRule Rule, const FActorEditContext& Context) -> bool
	{
		if (!Admit(Context, &Level)) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			if (!ValidateActors(Level, Actors) || !IsValid(Parent) || !ValidateActors(Level, {&Parent, 1}))
				return false;
			for (auto* Actor : Actors)
				if (WouldCreateCycle(*Actor, *Parent)) return false;

			TObjectPtr<AActor> Destination(Parent);
			return SubmitAttachment(Level, Actors, "Attach actors", [&](DSceneComponent& Root) {
				auto* Target = Destination.Get();
				if (!IsValid(Target) || Target->IsBeingDestroyed() || !Level.ContainsActor(Target)) return false;
				auto* ParentRoot = Target->GetRootComponent();
				if (!IsValid(ParentRoot) || WouldCreateCycle(*Root.GetOwner(), *Target)) return false;
				if (Root.GetAttachParent() == ParentRoot)
				{
					if (Rule != EAttachmentTransformRule::SnapToTarget) return false;
					const auto& Relative = Root.GetRelativeTransform();
					const FTransform Identity;
					if (Relative.Rotation == Identity.Rotation && Relative.Translation == Identity.Translation
						&& Relative.Scale3D == Identity.Scale3D) return false;
				}
				Root.SetFlags(EObjectFlags::Transactional);
				(void)Root.Modify(false);
				return Root.AttachToComponent(ParentRoot, Rule);
			});
		});
	}
	auto DEditorActorSubsystem::DetachActors(DLevel& Level, std::span<AActor* const> Actors,
		EDetachmentTransformRule Rule, const FActorEditContext& Context) -> bool
	{
		if (!Admit(Context, &Level)) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			if (!ValidateActors(Level, Actors)) return false;
			return SubmitAttachment(Level, Actors, "Detach actors", [&](DSceneComponent& Root) {
				if (!Root.GetAttachParent()) return false;
				Root.SetFlags(EObjectFlags::Transactional);
				(void)Root.Modify(false);
				return Root.DetachFromComponent(Rule);
			});
		});
	}
}
