#include "Actors/EditorActorSubsystem.h"
#include "DObject/StrongObjectPtr.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorEditingScope.h"
#include "Actors/ActorEditingValidation.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	namespace
	{
		struct FAttachmentState
		{
			TObjectPtr<DSceneComponent> Child, Parent;
			FTransform Relative;
		};

		auto EditActorMembership(DLevel& Level, std::span<AActor* const> Actors, bool bRemove,
			DTransactor& Transactions, std::string_view Description, std::vector<FAttachmentState> Links = {}) -> bool
		{
			std::vector<TStrongObjectPtr<AActor>> Pinned;
			std::vector<bool> Transient;
			for (auto* Actor : Actors)
			{
				if (!IsValid(Actor) || Actor->GetOuter() != &Level || Level.ContainsActor(Actor) != bRemove
					|| (!bRemove && !Level.IsActorNameAvailable(Actor->GetFName(), Actor))) return false;
				Pinned.emplace_back(Actor);
				Transient.push_back(Actor->HasAnyObjectFlags(EObjectFlags::Transient));
			}
			if (bRemove)
			{
				std::unordered_set<AActor*> Selected(Actors.begin(), Actors.end());
				for (const auto& Actor : Level.GetActors())
					for (const auto& Component : Actor->GetComponents())
						if (auto* Child = Cast<DSceneComponent>(Component.Get()); Child && Child->GetAttachParent()
							&& Selected.contains(Actor.Get()) != Selected.contains(Child->GetAttachParent()->GetOwner()))
							Links.push_back({Child, Child->GetAttachParent(), Child->GetRelativeTransform()});
			}
			FScopedTransaction Scope("Actor", Description, &Level);
			if (!Scope.IsOutstanding()) return false;
			Level.SetFlags(EObjectFlags::Transactional);
			Level.Modify(false);
			for (auto* Actor : Actors)
			{
				if (!Transactions.IsActive()) return false;
				if (!bRemove) Actor->SetTransient(true);
				Actor->SetFlags(EObjectFlags::Transactional);
				// Presence changes the Actor and Level. Capture a root separately only
				// when its attachment changes, leaving configured component assets live.
				Actor->DObject::Modify(false);
			}
			for (const auto& Link : Links)
			{
				if (!Transactions.IsActive()) return false;
				Link.Child->SetFlags(EObjectFlags::Transactional);
				Link.Child->Modify(false);
			}
			if (!Transactions.IsActive()) return false;
			if (bRemove)
				for (const auto& Link : Links)
					if (!Link.Child->DetachFromComponent(EDetachmentTransformRule::KeepWorld)) return false;
			for (size_t I = 0; I < Actors.size(); ++I)
			{
				const size_t Index = bRemove ? Actors.size() - 1 - I : I;
				auto* Actor = Actors[Index];
				if (!Transactions.IsActive() || !CanEditActorLevel(&Level) || !IsValid(Actor)) return false;
				if (!bRemove) Actor->SetTransient(Transient[Index]);
				if (!Level.SetActorPresence(Actor, !bRemove, std::numeric_limits<size_t>::max(), Links.empty()) || !IsValid(Actor) || Level.ContainsActor(Actor) == bRemove) return false;
				if (bRemove) Actor->SetTransient(true);
				if (!CanEditActorLevel(&Level)) return false;
			}
			if (!bRemove && !Links.empty())
			{
				for (const auto& Link : Links)
				{
					if (!IsValid(Link.Child.Get()) || !IsValid(Link.Parent.Get())
						|| !Link.Child->AttachToComponent(Link.Parent.Get(), EAttachmentTransformRule::KeepRelative)) return false;
					Link.Child->SetRelativeTransform(Link.Relative);
				}
				for (auto* Actor : Actors)
				{
					if (!CanEditActorOwner(Actor)) return false;
					for (const auto& Component : Actor->GetComponentsSnapshot())
					{
						if (IsValid(Component.Get()) && Actor->OwnsComponent(Component.Get()) && Level.GetWorld())
							Component->RegisterComponent();
						if (!CanEditActorOwner(Actor)) return false;
					}
				}
			}
			return true;
		}
	}

	static auto SpawnConfiguredActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
		DTransactor& Transactions, auto&& ConfigureAndValidate,
		ESpawnActorNameMode NameMode, const std::string& Label) -> std::expected<AActor*, std::string>
	{
		if (!CanEditActorLevel(&Level) || !CanConstructObjectOfClass(Class, AActor::StaticClass()))
			return std::unexpected(std::string("The actor class or target level is unavailable."));
		DObjectForceRegistration(Class);
		if (Label.find('\0') != std::string::npos) return std::unexpected(std::string("Invalid actor label."));
		const FName Requested = Name.IsNone()
			? FName(Class->GetDefaultObjectName().empty() ? Class->GetName() : Class->GetDefaultObjectName()) : Name;
		Name = Level.ResolveActorName(Requested, Name.IsNone() ? ESpawnActorNameMode::Requested : NameMode);
		if (Name.IsNone()) return std::unexpected(std::string("The requested actor name is occupied."));
		AActor* Raw = nullptr;
		TStrongObjectPtr<AActor> Prepared;
		{
			FScopedPackageDirtySuppression SuppressPreparation;
			Raw = NewObject<AActor>(Class, &Level, Name, EObjectFlags::Transactional);
			Prepared = Raw;
			auto Reject = [&](std::string Reason) {
				if (IsValid(Raw)) MarkObjectHierarchyAsGarbage(Raw);
				return std::unexpected(std::move(Reason));
			};
			if (!Raw) return Reject("Actor construction failed.");
			Raw->SetInitialActorLabel(Level.MakeUniqueActorLabel(Label.empty() ? Requested.ToString() : Label));
			if (!Raw->RerunConstructionScripts()) return Reject("Actor construction scripts failed.");
			Raw->SetActorTransform(Transform);
			try
			{
				auto Result = ConfigureAndValidate(*Raw);
				if (!Result) return Reject(std::move(Result.error()));
			}
			catch (const std::exception& Error) { return Reject(Error.what()); }
			catch (...) { return Reject("Actor configuration threw."); }
			if (!IsValid(Raw)) return Reject("Actor retired during configuration.");
		}
		if (!EditActorMembership(Level, {&Raw, 1}, false, Transactions, "Add actor"))
		{
			if (IsValid(Raw) && !Level.ContainsActor(Raw)) MarkObjectHierarchyAsGarbage(Raw);
			return std::unexpected(std::string("Actor unavailable for publication."));
		}
		return Raw;
	}

	auto DEditorActorSubsystem::SpawnActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
		const FActorEditContext& Context, std::function<void(AActor&)> Configure, ESpawnActorNameMode NameMode, const std::string& Label, std::string* OutError) -> AActor*
	{
		if (OutError) OutError->clear();
		auto Admission = Admit(Context, &Level, nullptr, false, OutError);
		if (!Admission) return nullptr;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> AActor*
		{
			auto Result = SpawnConfiguredActor(Level, Class, Name, Transform, *Admission, [&](AActor& Actor) -> std::expected<void, std::string> {
				if (Configure) Configure(Actor);
				std::string Reason;
				if (!Admit(Context, &Level, nullptr, true, &Reason)) return std::unexpected(std::move(Reason));
				return {};
			}, NameMode, Label);
			if (!Result && OutError) *OutError = Result.error();
			return Result.value_or(nullptr);
		});
	}
	auto DEditorActorSubsystem::DestroyActors(DLevel& Level, std::span<AActor* const> Actors, const FActorEditContext& Context) -> bool
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			auto* Transactions = Admission;
			if (Actors.empty()) return false;
			std::unordered_set<AActor*> Unique;
			for (AActor* Actor : Actors)
				if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Unique.insert(Actor).second) return false;
			return EditActorMembership(Level, Actors, true, *Transactions, "Delete actors");

		});
	}
	auto DEditorActorSubsystem::DuplicateActors(DLevel& Level, std::span<AActor* const> Actors, const FActorEditContext& Context) -> std::vector<TObjectPtr<AActor>>
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return {};
		return RunActorEdit(GetEditor(), bEditing, [&]() -> std::vector<TObjectPtr<AActor>>
		{
			auto* Transactions = Admission;
			if (Actors.empty()) return {};
			std::unordered_set<AActor*> Unique;
			for (AActor* Actor : Actors)
				if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Unique.insert(Actor).second) return {};
			std::vector<TObjectPtr<AActor>> Copies;
			std::vector<AActor*> Raw;
			std::vector<FAttachmentState> PreparedAttachments;
			{
				FScopedPackageDirtySuppression SuppressPreparation;
				std::unordered_map<DObject*, DObject*> Duplicates;
				std::vector<std::string> Labels;
				for (AActor* Source : Actors)
				{
					std::unordered_map<DObject*, DObject*> Graph;
					auto Copy = DuplicateObject(Source, &Level, Level.MakeUniqueActorName(Source->GetFName()), &Graph);
					if (!Copy) { for (const auto& Actor : Copies) MarkObjectHierarchyAsGarbage(Actor.Get()); return {}; }
					Labels.push_back(Level.MakeUniqueActorLabel(Source->GetActorLabel(), nullptr, Labels));
					(*Copy)->SetInitialActorLabel(Labels.back());
					Copies.push_back(*Copy);
					Raw.push_back(*Copy);
					Duplicates.insert(Graph.begin(), Graph.end());
					// Duplication remaps the complete owned graph and runs PostLoad before publication.
				}
				const std::unordered_set<AActor*> CopiedOwners(Raw.begin(), Raw.end());
				// Reconnect references between selected graphs as well as within each Outer tree.
				for (const auto& [Source, Copy] : Duplicates)
				{
					if (!IsValid(Copy)) continue;
					if (const auto Result = CopyEditableObjectProperties(Source, Copy, Duplicates); !Result)
					{ for (const auto& Actor : Copies) MarkObjectHierarchyAsGarbage(Actor.Get()); return {}; }
					if (auto* Scene = Cast<DSceneComponent>(Copy))
					{
						auto* Parent = static_cast<DSceneComponent*>(Source)->GetAttachParent();
						if (auto It = Duplicates.find(Parent); It != Duplicates.end()) Parent = static_cast<DSceneComponent*>(It->second);
						if (Parent && !CopiedOwners.contains(Parent->GetOwner()))
						{
							const auto Relative = Scene->GetRelativeTransform();
							PreparedAttachments.push_back({Scene, Parent, Relative});
							if (Scene->GetAttachParent()) Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
							Scene->SetWorldTransform(FTransform::Combine(Parent->GetWorldTransform(), Relative));
						}
						else if (Parent) verify(Scene->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative));
						else { if (Scene->GetAttachParent()) Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative); Scene->UpdateComponentToWorld(); }
					}
				}
				// Rebuild derived data after editable cross-graph references are remapped.
				for (AActor* Actor : Raw)
					for (const auto& Component : Actor->GetComponentsSnapshot())
						if (IsValid(Component.Get()) && Component->GetCreationMethod() != EComponentCreationMethod::Generated) Component->PostLoad();
				// External parent child lists are changed only by successful replay.
				for (AActor* Actor : Raw)
				{
					for (const auto& Component : Actor->GetComponents())
						if (auto* Scene = Cast<DSceneComponent>(Component.Get()); Scene && Scene->GetAttachParent() && !CopiedOwners.contains(Scene->GetAttachParent()->GetOwner()))
							Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
				}
			}
			if (auto Revalidated = Admit(Context, &Level, nullptr, true); !Revalidated)
			{ for (const auto& Actor : Copies) if (IsValid(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get()); return {}; }
			if (!EditActorMembership(Level, Raw, false, *Transactions, "Duplicate actors", std::move(PreparedAttachments)))
			{
				for (const auto& Actor : Copies)
					if (IsValid(Actor.Get()) && !Level.ContainsActor(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get());
				return {};
			}
			return Copies;

		});
	}
	auto DEditorActorSubsystem::SetActorLabel(DLevel& Level, AActor* Actor, const std::string& Label,
		const FActorEditContext& Context, bool bMakeUnique) -> bool
	{
		auto* Transactions = Admit(Context, &Level);
		if (!Transactions) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsTemplateObject() || Label.find('\0') != std::string::npos) return false;
			const auto* Class = Actor->GetClass();
			std::string Resolved = Label.empty()
				? (Class->GetDefaultObjectName().empty() ? Class->GetName() : Class->GetDefaultObjectName()) : Label;
			if (bMakeUnique) Resolved = Level.MakeUniqueActorLabel(Resolved, Actor);
			if (Actor->GetActorLabel() == Resolved) return true;
			const bool bOwnsRecording = !Transactions->IsActive();
			FScopedTransaction Recording("ActorLabel", "Set actor label", Actor);
			if (!Recording.IsOutstanding()) return false;
			Actor->SetFlags(EObjectFlags::Transactional);
			if (!Actor->Modify(false) || !Transactions->IsActive())
			{
				if (bOwnsRecording) Recording.Cancel();
				return false;
			}
			return Actor->SetActorLabel(Resolved);
		});
	}

	auto DEditorActorSubsystem::RenameActor(DLevel& Level, AActor* Actor, FName Name, const FActorEditContext& Context, ESpawnActorNameMode NameMode) -> bool
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			auto* Transactions = Admission;
			if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsTemplateObject() || Name.IsNone()) return false;
			Name = Level.ResolveActorName(Name, NameMode, Actor);
			if (Name.IsNone()) return false;
			if (Actor->GetFName() == Name) return true;
			const bool bOwnsRecording = !Transactions->IsActive();
			FScopedTransaction Recording("ActorRename", "Rename actor", Actor);
			if (!Recording.IsOutstanding()) return false;
			Actor->SetFlags(EObjectFlags::Transactional);
			if (!Actor->Modify(false) || !Transactions->IsActive())
			{
				if (bOwnsRecording) Recording.Cancel();
				return false;
			}
			return Level.RenameActor(Actor, Name, ESpawnActorNameMode::RequiredReturnNull);

		});
	}
}
