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
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace Durin
{
	// Keep the same child-before-Outer PostLoad order as DuplicateObject. Capture the
	// created graph before callbacks can rebuild components or collect an Outer-only child.
	static auto GatherDuplicatePostLoad(DObject* Source, const std::unordered_map<DObject*, DObject*>& Graph,
		std::vector<TStrongObjectPtr<DObject>>& Copies) -> void
	{
		auto It = Graph.find(Source);
		if (It == Graph.end()) return;
		Copies.emplace_back(It->second);
		for (auto* Inner : GDObjectArray.GetObjectsWithOuter(Source, EObjectQueryScope::LiveOnly))
			GatherDuplicatePostLoad(Inner, Graph, Copies);
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
			Raw = NewObject<AActor>(Class, &Level, Name, EObjectFlags::Transactional | EObjectFlags::Transient);
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
		FScopedTransaction Scope("Actor", "Add actor", &Level);
		if (Scope.IsOutstanding())
		{
			Level.SetFlags(EObjectFlags::Transactional);
			Level.Modify(false);
			if (IsValid(Raw)) Raw->SetTransient(false);
			if (Transactions.IsActive() && Level.AddActor(Raw))
			{
				if (IsValid(Raw) && Level.ContainsActor(Raw)) return Raw;
				return std::unexpected(std::string("Actor removed during publication."));
			}
		}
		if (IsValid(Raw) && !Level.ContainsActor(Raw)) MarkObjectHierarchyAsGarbage(Raw);
		return std::unexpected(std::string("Actor unavailable for publication."));
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
			std::vector<DSceneComponent*> BoundaryChildren;
			for (const auto& Actor : Level.GetActors())
				for (const auto& Component : Actor->GetComponents())
					if (auto* Child = Cast<DSceneComponent>(Component.Get()); Child && Child->GetAttachParent()
						&& Unique.contains(Actor.Get()) != Unique.contains(Child->GetAttachParent()->GetOwner()))
						BoundaryChildren.push_back(Child);

			FScopedTransaction Scope("Actor", "Delete actors", &Level);
			if (!Scope.IsOutstanding()) return false;
			Level.SetFlags(EObjectFlags::Transactional);
			Level.Modify(false);
			for (auto* Actor : Actors)
			{
				if (!Transactions->IsActive() || !IsValid(Actor)) return false;
				Actor->SetFlags(EObjectFlags::Transactional);
				Actor->DObject::Modify(false);
			}
			for (auto* Child : BoundaryChildren)
			{
				if (!Transactions->IsActive() || !IsValid(Child)) return false;
				Child->SetFlags(EObjectFlags::Transactional);
				Child->Modify(false);
			}
			for (auto* Child : BoundaryChildren)
			{
				if (!Transactions->IsActive() || !CanEditActorLevel(&Level) || !IsValid(Child)
					|| !Child->DetachFromComponent(EDetachmentTransformRule::KeepWorld)) return false;
			}
			for (auto* Actor : Actors | std::views::reverse)
			{
				if (!Transactions->IsActive() || !CanEditActorLevel(&Level) || !IsValid(Actor)) return false;
				// Removal notifications may cancel history and collect after the Level releases its reference.
				TStrongObjectPtr<AActor> Removing(Actor);
				if (!Level.RemoveActor(Actor) || !IsValid(Actor) || Level.ContainsActor(Actor)) return false;
				Actor->SetTransient(true);
				if (!CanEditActorLevel(&Level)) return false;
			}
			return true;

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
			std::vector<TStrongObjectPtr<AActor>> Prepared;
			std::unordered_map<AActor*, FName> ParentNames;
			std::unordered_map<FName, TObjectPtr<AActor>> CopiesBySourceName;
			auto DiscardUnpublished = [&]() -> std::vector<TObjectPtr<AActor>> {
				for (const auto& Actor : Prepared)
					if (Actor && !Level.ContainsActor(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get());
				return {};
			};
			{
				std::unordered_map<DObject*, DObject*> Duplicates;
				std::vector<std::string> Labels;
				std::vector<std::vector<TStrongObjectPtr<DObject>>> PendingPostLoad;
				for (AActor* Source : Actors)
				{
					std::unordered_map<DObject*, DObject*> Graph;
					if (!IsValid(Source)) return DiscardUnpublished();
					auto* SourceParent = Source->GetAttachParentActor();
					const FName ParentName = SourceParent ? SourceParent->GetFName() : NAME_None;
					auto Result = StaticDuplicateObjectEx({.SourceObject = Source, .DestOuter = &Level,
						.DestName = Level.MakeUniqueActorName(Source->GetFName()), .bSkipPostLoad = true,
						.CreatedObjects = &Graph, .ApplyFlags = EObjectFlags::Transient});
					if (!Result) continue;
					auto* Copy = static_cast<AActor*>(*Result);
					GatherDuplicatePostLoad(Source, Graph, PendingPostLoad.emplace_back());
					Labels.push_back(Level.MakeUniqueActorLabel(Source->GetActorLabel(), nullptr, Labels));
					Copy->SetInitialActorLabel(Labels.back());
					Prepared.emplace_back(Copy);
					if (!ParentName.IsNone()) ParentNames.emplace(Copy, ParentName);
					CopiesBySourceName.emplace(Source->GetFName(), Copy);
					Duplicates.insert(Graph.begin(), Graph.end());
				}

				if (Prepared.empty()) return {};

				// Reconnect references between selected graphs as well as within each Outer tree.
				for (const auto& [Source, Copy] : Duplicates)
				{
					if (!IsValid(Copy)) continue;
					if (const auto Result = CopyEditableObjectProperties(Copy, Copy, Duplicates); !Result)
						return DiscardUnpublished();
					if (auto* Scene = Cast<DSceneComponent>(Copy))
					{
						auto* SourceScene = static_cast<DSceneComponent*>(Source);
						auto* Parent = SourceScene->GetAttachParent();
						if (Parent && Parent->GetOwner() != SourceScene->GetOwner())
						{
							if (Scene->GetAttachParent()) Scene->DetachFromComponent(EDetachmentTransformRule::KeepWorld);
							Scene->SetWorldTransform(SourceScene->GetWorldTransform());
						}
						else if (auto It = Duplicates.find(Parent); It != Duplicates.end())
							verify(Scene->AttachToComponent(static_cast<DSceneComponent*>(It->second), EAttachmentTransformRule::KeepRelative));
						else { if (Scene->GetAttachParent()) Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative); Scene->UpdateComponentToWorld(); }
					}
				}
				for (const auto& Graph : PendingPostLoad)
				{
					const FObjectPtr Root(Graph.front().Get());
					for (const auto& Object : Graph | std::views::reverse)
					{
						if (!Root.IsValid()) break;
						if (IsValid(Object.Get())) Object->PostLoad();
					}
					if (!Root.IsValid()) MarkObjectHierarchyAsGarbage(Root.Get());
				}
			}
			std::erase_if(Prepared, [](const auto& Actor) { return !IsValid(Actor.Get()); });
			if (Prepared.empty()) return {};
			if (auto Revalidated = Admit(Context, &Level, nullptr, true); !Revalidated)
				return DiscardUnpublished();
			FScopedTransaction Scope("Actor", "Duplicate actors", &Level);
			if (!Scope.IsOutstanding()) return DiscardUnpublished();
			Level.SetFlags(EObjectFlags::Transactional);
			Level.Modify(false);
			std::vector<TObjectPtr<AActor>> Published;
			for (AActor* Actor : Prepared)
			{
				if (IsValid(Actor)) Actor->SetTransient(false);
				if (Transactions->IsActive() && CanEditActorLevel(&Level) && Level.AddActor(Actor))
					Published.emplace_back(Actor);
				else if (IsValid(Actor) && !Level.ContainsActor(Actor))
					MarkObjectHierarchyAsGarbage(Actor);
			}

			for (AActor* Child : Published)
			{
				if (!Transactions->IsActive() || !CanEditActorLevel(&Level)) break;
				auto Link = ParentNames.find(Child);
				if (Link == ParentNames.end() || !CanEditActorOwner(Child)) continue;
				TObjectPtr<DSceneComponent> Root = Child->GetRootComponent();
				if (!Root.IsValid()) continue;
				Root->SetFlags(EObjectFlags::Transactional);
				Root->Modify(false);
				if (!Transactions->IsActive() || !CanEditActorLevel(&Level)) break;
				if (!CanEditActorOwner(Child) || !Root.IsValid() || Child->GetRootComponent() != Root.Get()) continue;
				const auto It = CopiesBySourceName.find(Link->second);
				auto* ParentActor = It != CopiesBySourceName.end() ? It->second.Get() : nullptr;
				if (!IsValid(ParentActor) || !Level.ContainsActor(ParentActor))
					ParentActor = Level.FindActorByName(Link->second);
				if (!CanEditActorOwner(ParentActor)) continue;
				auto* Parent = ParentActor->GetRootComponent();
				if (IsValid(Parent)) Root->AttachToComponent(Parent, EAttachmentTransformRule::KeepWorld);
			}
			std::erase_if(Published, [&](const auto& Actor) { return !Actor.IsValid() || !Level.ContainsActor(Actor.Get()); });
			return Published;

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
			if (!Actor->SetActorLabel(Resolved))
			{
				if (bOwnsRecording) Recording.Cancel();
				return false;
			}
			return true;
		});
	}

	auto DEditorActorSubsystem::RenameActor(DLevel& Level, AActor* Actor, FName Name, const FActorEditContext& Context, ESpawnActorNameMode NameMode) -> bool
	{
		if (!Admit(Context, &Level)) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsTemplateObject() || Name.IsNone()) return false;
			Name = Level.ResolveActorName(Name, NameMode, Actor);
			if (Name.IsNone()) return false;
			if (Actor->GetFName() == Name) return true;
			FScopedTransaction Recording("ActorRename", "Rename actor", Actor);
			if (!Recording.IsOutstanding()) return false;
			Actor->SetFlags(EObjectFlags::Transactional);
			(void)Actor->Modify();
			return Level.RenameActor(Actor, Name, ESpawnActorNameMode::RequiredReturnNull, ERenameFlags::DoNotDirty);

		});
	}
}
