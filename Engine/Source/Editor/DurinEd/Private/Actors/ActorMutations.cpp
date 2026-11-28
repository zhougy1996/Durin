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

	static auto PrepareActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
		auto&& ConfigureAndValidate,
		ESpawnActorNameMode NameMode, const std::string& Label) -> std::expected<TStrongObjectPtr<AActor>, std::string>
	{
		if (!CanConstructObjectOfClass(Class, AActor::StaticClass()))
			return std::unexpected(std::string("The actor class is unavailable."));
		DObjectForceRegistration(Class);
		if (Label.find('\0') != std::string::npos) return std::unexpected(std::string("Invalid actor label."));
		const FName Requested = Name.IsNone()
			? FName(Class->GetDefaultObjectName().empty() ? Class->GetName() : Class->GetDefaultObjectName()) : Name;
		Name = Level.ResolveActorName(Requested, Name.IsNone() ? ESpawnActorNameMode::Requested : NameMode);
		if (Name.IsNone()) return std::unexpected(std::string("The requested actor name is occupied."));
		TStrongObjectPtr<AActor> Prepared(NewObject<AActor>(Class, &Level, Name, EObjectFlags::Transactional | EObjectFlags::Transient));
		auto* Raw = Prepared.Get();
		if (!Raw) return std::unexpected(std::string("Actor construction failed."));
		auto Reject = [&](std::string Reason) {
			if (IsValid(Raw)) MarkObjectHierarchyAsGarbage(Raw);
			return std::unexpected(std::move(Reason));
		};
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
		return Prepared;
	}

	auto DEditorActorSubsystem::SpawnActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
		const FActorEditContext& Context, std::function<void(AActor&)> Configure, ESpawnActorNameMode NameMode, const std::string& Label,
		std::string* OutError) -> AActor*
	{
		if (OutError) OutError->clear();
		return RunActorEdit(Context, &Level, nullptr, [&]() -> AActor* {
			TStrongObjectPtr<AActor> Spawned;
			{
				FScopedTransaction Scope("Actor", "Add actor", &Level);
				if (!Scope.IsOutstanding())
				{
					if (OutError) *OutError = "Unable to start the actor transaction.";
					return nullptr;
				}
				Spawned = SpawnConfiguredActor(Level, Class, Name, Transform, Context, Configure, NameMode, Label, OutError);
				if (!Spawned) return nullptr;
			}
			if (IsValid(Spawned.Get()) && Level.ContainsActor(Spawned.Get())) return Spawned.Get();
			if (OutError) *OutError = "Actor removed during publication.";
			return nullptr;
		}, OutError);
	}

	auto DEditorActorSubsystem::SpawnConfiguredActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
		const FActorEditContext& Context, const std::function<void(AActor&)>& Configure, ESpawnActorNameMode NameMode,
		const std::string& Label, std::string* OutError) -> AActor*
	{
		auto Prepared = PrepareActor(Level, Class, Name, Transform, [&](AActor& Actor) -> std::expected<void, std::string> {
			if (Configure) Configure(Actor);
			std::string Reason;
			if (!ValidateEditTarget(Context, &Level, nullptr, &Reason)) return std::unexpected(std::move(Reason));
			return {};
		}, NameMode, Label);
		if (!Prepared)
		{
			if (OutError) *OutError = Prepared.error();
			return nullptr;
		}
		auto* Actor = Prepared->Get();
		Level.SetFlags(EObjectFlags::Transactional);
		Level.Modify(false);
		if (IsValid(Actor)) Actor->SetTransient(false);
		if (Level.AddActor(Actor))
		{
			if (IsValid(Actor) && Level.ContainsActor(Actor)) return Actor;
			if (OutError) *OutError = "Actor removed during publication.";
			return nullptr;
		}
		if (IsValid(Actor) && !Level.ContainsActor(Actor)) MarkObjectHierarchyAsGarbage(Actor);
		if (OutError) *OutError = "Actor unavailable for publication.";
		return nullptr;
	}
	auto DEditorActorSubsystem::DestroyActors(DLevel& Level, std::span<AActor* const> Actors, const FActorEditContext& Context) -> bool
	{
		return RunActorEdit(Context, &Level, nullptr, [&]() -> bool
		{
			if (Actors.empty()) return false;
			FScopedTransaction Scope("Actor", "Delete actors", &Level);
			if (!Scope.IsOutstanding() || !CanEditActorLevel(&Level)) return false;
			std::unordered_set<AActor*> Unique;
			for (AActor* Actor : Actors)
				if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Unique.insert(Actor).second) return false;
			std::vector<DSceneComponent*> BoundaryChildren;
			for (const auto& Actor : Level.GetActors())
				for (const auto& Component : Actor->GetComponents())
					if (auto* Child = Cast<DSceneComponent>(Component.Get()); Child && Child->GetAttachParent()
						&& Unique.contains(Actor.Get()) != Unique.contains(Child->GetAttachParent()->GetOwner()))
						BoundaryChildren.push_back(Child);

			Level.SetFlags(EObjectFlags::Transactional);
			Level.Modify(false);
			for (auto* Actor : Actors)
			{
				if (!IsValid(Actor)) return false;
				Actor->SetFlags(EObjectFlags::Transactional);
				Actor->DObject::Modify(false);
			}
			for (auto* Child : BoundaryChildren)
			{
				if (!IsValid(Child)) return false;
				Child->SetFlags(EObjectFlags::Transactional);
				Child->Modify(false);
			}
			for (auto* Child : BoundaryChildren)
			{
				if (!CanEditActorLevel(&Level) || !IsValid(Child)
					|| !Child->DetachFromComponent(EDetachmentTransformRule::KeepWorld)) return false;
			}
			for (auto* Actor : Actors | std::views::reverse)
			{
				if (!CanEditActorLevel(&Level) || !IsValid(Actor)) return false;
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
		return RunActorEdit(Context, &Level, nullptr, [&]() -> std::vector<TObjectPtr<AActor>>
		{
			if (Actors.empty()) return {};
			FScopedTransaction Scope("Actor", "Duplicate actors", &Level);
			if (!Scope.IsOutstanding() || !CanEditActorLevel(&Level)) return {};
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
			if (auto Revalidated = ValidateEditTarget(Context, &Level, nullptr); !Revalidated)
				return DiscardUnpublished();
			Level.SetFlags(EObjectFlags::Transactional);
			Level.Modify(false);
			std::vector<TObjectPtr<AActor>> Published;
			for (AActor* Actor : Prepared)
			{
				if (IsValid(Actor)) Actor->SetTransient(false);
				if (CanEditActorLevel(&Level) && Level.AddActor(Actor))
					Published.emplace_back(Actor);
				else if (IsValid(Actor) && !Level.ContainsActor(Actor))
					MarkObjectHierarchyAsGarbage(Actor);
			}

			for (AActor* Child : Published)
			{
				if (!CanEditActorLevel(&Level)) break;
				auto Link = ParentNames.find(Child);
				if (Link == ParentNames.end() || !CanEditActorOwner(Child)) continue;
				TObjectPtr<DSceneComponent> Root = Child->GetRootComponent();
				if (!Root.IsValid()) continue;
				Root->SetFlags(EObjectFlags::Transactional);
				Root->Modify(false);
				if (!CanEditActorLevel(&Level)) break;
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
		return RunActorEdit(Context, &Level, nullptr, [&]() -> bool
		{
			if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsTemplateObject() || Label.find('\0') != std::string::npos) return false;
			const auto* Class = Actor->GetClass();
			std::string Resolved = Label.empty()
				? (Class->GetDefaultObjectName().empty() ? Class->GetName() : Class->GetDefaultObjectName()) : Label;
			if (bMakeUnique) Resolved = Level.MakeUniqueActorLabel(Resolved, Actor);
			if (Actor->GetActorLabel() == Resolved) return true;
			FScopedTransaction Recording("ActorLabel", "Set actor label", Actor);
			if (!Recording.IsOutstanding() || !CanEditActorOwner(Actor) || Actor->GetOuter() != &Level) return false;
			Actor->SetFlags(EObjectFlags::Transactional);
			return Actor->SetActorLabel(Resolved);
		});
	}

	auto DEditorActorSubsystem::RenameActor(DLevel& Level, AActor* Actor, FName Name, const FActorEditContext& Context, ESpawnActorNameMode NameMode) -> bool
	{
		return RunActorEdit(Context, &Level, nullptr, [&]() -> bool
		{
			if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsTemplateObject() || Name.IsNone()) return false;
			Name = Level.ResolveActorName(Name, NameMode, Actor);
			if (Name.IsNone()) return false;
			if (Actor->GetFName() == Name) return true;
			FScopedTransaction Recording("ActorRename", "Rename actor", Actor);
			if (!Recording.IsOutstanding() || !CanEditActorOwner(Actor) || Actor->GetOuter() != &Level) return false;
			Actor->SetFlags(EObjectFlags::Transactional);
			(void)Actor->Modify();
			return Level.RenameActor(Actor, Name, ESpawnActorNameMode::RequiredReturnNull, ERenameFlags::DoNotDirty);

		});
	}
}
