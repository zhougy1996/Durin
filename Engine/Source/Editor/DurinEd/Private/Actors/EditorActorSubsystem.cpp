#include "Actors/EditorActorSubsystem.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorEditingScope.h"
#include "Actors/ActorEditingValidation.h"
#include "Components/SceneComponent.h"
#include "Editor/EditorEngine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "DObject/ObjectLifecycle.h"
#include "Tools/EditorScriptCall.h"

namespace Durin
{
	DEditorActorSubsystem::DEditorActorSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}

	auto DEditorActorSubsystem::CanEditActor(AActor& Actor, const FActorEditContext& Context) const -> std::expected<void, std::string>
	{
		std::string Error;
		if (const char* Reason = ActorEditBlockReason(bEditing)) return std::unexpected(Reason);
		if (!ValidateEditTarget(Context, nullptr, &Actor, &Error)) return std::unexpected(std::move(Error));
		return {};
	}

	static auto RejectEdit(std::string* OutError, const char* Message) -> bool
	{
		if (OutError) *OutError = Message;
		return false;
	}

	auto DEditorActorSubsystem::ValidateEditTarget(const FActorEditContext& Context, DLevel* Level, AActor* Owner, std::string* OutError) const -> bool
	{
		if (!IsValid(this)) return RejectEdit(OutError, "The actor subsystem is unavailable.");
		if (Owner)
		{
			if (!IsValid(Owner)) return RejectEdit(OutError, "The actor is unavailable.");
			auto* OwnerLevel = Cast<DLevel>(Owner->GetOuter());
			if (Level && Level != OwnerLevel) return RejectEdit(OutError, "The actor belongs to another level.");
			Level = OwnerLevel;
		}
		if (Context.bReadOnly) return RejectEdit(OutError, "The level is read-only.");
		auto* Host = GetEditor();
		if (!IsValid(Host) || Host != GEditor || Host->IsShutdownRequested()
			|| Host->GetEditorSubsystem<DEditorActorSubsystem>() != this)
			return RejectEdit(OutError, "The editor host is unavailable or shutting down.");
		if (Host->IsPlaying()) return RejectEdit(OutError, "Actor editing is unavailable during play.");
		auto* World = Host->GetEditorWorld();
		if (!IsValid(Level) || !IsValid(World) || Level->GetWorld() != World || World->GetCurrentLevel() != Level)
			return RejectEdit(OutError, "The target is not the current editor level.");
		if (Level->IsTemplateObject()
			|| (Owner && (Owner->IsTemplateObject() || Owner->IsBeingDestroyed() || Owner->HasBegunPlay()
				|| Owner->GetOuter() != Level || !Level->ContainsActor(Owner))))
			return RejectEdit(OutError, "The level or actor is unavailable for editing.");
		if (World->IsShutdownRequested())
			return RejectEdit(OutError, "The world is shutting down.");
		if (World->GetPlayState() != EWorldPlayState::Stopped)
			return RejectEdit(OutError, "The world is playing.");
		if (auto* Script = FScopedEditorScriptCall::Current())
		{
			if (!Script->IsAdmitted() || Script->GetContext().Host != Host)
				return RejectEdit(OutError, "The script is not admitted by this editor.");
			const auto Access = Script->ResolveEditAccess(*Level);
			if (Access != EEditorScriptTargetAccess::Writable)
				return RejectEdit(OutError, "The script cannot write to this level.");
		}
		return true;
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
			Edit Apply) -> bool
		{
			// Notifications may retire a later actor before its turn.
			std::vector<TObjectPtr<AActor>> Targets(Actors.begin(), Actors.end());
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
		return RunActorEdit(Context, &Level, nullptr, [&]() -> bool
		{
			FScopedTransaction Scope("ActorAttachment", "Attach actors", nullptr);
			if (!Scope.IsOutstanding() || !CanEditActorLevel(&Level)) return false;
			if (!ValidateActors(Level, Actors) || !IsValid(Parent) || !ValidateActors(Level, {&Parent, 1}))
				return false;
			for (auto* Actor : Actors)
				if (WouldCreateCycle(*Actor, *Parent)) return false;

			TObjectPtr<AActor> Destination(Parent);
			return SubmitAttachment(Level, Actors, [&](DSceneComponent& Root) {
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
		return RunActorEdit(Context, &Level, nullptr, [&]() -> bool
		{
			FScopedTransaction Scope("ActorAttachment", "Detach actors", nullptr);
			if (!Scope.IsOutstanding() || !CanEditActorLevel(&Level)) return false;
			if (!ValidateActors(Level, Actors)) return false;
			return SubmitAttachment(Level, Actors, [&](DSceneComponent& Root) {
				if (!Root.GetAttachParent()) return false;
				Root.SetFlags(EObjectFlags::Transactional);
				(void)Root.Modify(false);
				return Root.DetachFromComponent(Rule);
			});
		});
	}
}
