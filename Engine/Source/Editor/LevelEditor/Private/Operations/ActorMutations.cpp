#include "Operations/ActorMutations.h"

#include "Actors/CameraActor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Transactions/Transactor.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto CanEdit(DLevel& Level, bool bReadOnly = false) -> bool
		{
			return !bReadOnly && IsValid(&Level) && !Level.IsTemplateObject()
				&& (!Level.GetWorld() || (!Level.GetWorld()->HasBegunPlay() && !Level.GetWorld()->IsEndingPlay()));
		}
		auto UniqueName(DLevel& Level, FName Requested, AActor* Ignored = nullptr) -> FName
		{
			const auto Objects = GDObjectArray.GetObjectsWithOuter(&Level, EObjectQueryScope::LiveOnly);
			FName Name = Requested;
			for (uint32 Suffix = 2; std::ranges::any_of(Objects, [&](DObject* Object) { return Object != Ignored && Object->GetFName() == Name; }); ++Suffix)
				Name = FName(std::format("{}_{}", Requested, Suffix));
			return Name;
		}
		struct FActorState
		{
			TObjectPtr<AActor> Actor;
			FName Name;
			size_t Index;
			bool bTransient, bTick;
			std::vector<TObjectPtr<DActorComponent>> Registered;
		};
		struct FAttachmentState
		{
			TObjectPtr<DSceneComponent> Child, Parent;
			FTransform Relative;
		};

		class FActorPresenceChange final : public ITransactionCustomChange
		{
		public:
			FActorPresenceChange(DLevel& InLevel, std::span<AActor* const> InActors, bool bInRemove, std::string InDescription)
				: Level(&InLevel), Description(std::move(InDescription)), bRemove(bInRemove), BeforeCamera(InLevel.GetPrimaryCameraActor())
			{
				Packages[0] = InLevel.GetPackage();
				std::unordered_set<AActor*> Selected(InActors.begin(), InActors.end());
				for (AActor* Actor : InActors)
				{
					size_t Index = InLevel.GetActors().size() + States.size();
					if (bRemove) Index = static_cast<size_t>(std::ranges::find_if(InLevel.GetActors(), [Actor](const auto& Entry) { return Entry.Get() == Actor; }) - InLevel.GetActors().begin());
					States.push_back({Actor, Actor->GetFName(), Index, Actor->HasAnyObjectFlags(EObjectFlags::Transient),
						bRemove ? Actor->GetPrimaryActorTick().IsTickFunctionRegistered() : InLevel.GetWorld() != nullptr, {}});
					for (const auto& Component : Actor->GetComponents())
						if (Component && (bRemove ? Component->IsRegistered() : InLevel.GetWorld() != nullptr)) States.back().Registered.push_back(Component);
				}
				std::ranges::sort(States, {}, &FActorState::Index);
				std::vector<AActor*> All;
				for (const auto& Actor : InLevel.GetActors()) All.push_back(Actor.Get());
				if (!bRemove) All.insert(All.end(), InActors.begin(), InActors.end());
				for (AActor* Actor : All)
					for (const auto& Component : Actor->GetComponents())
						if (auto* Child = Cast<DSceneComponent>(Component.Get()); Child && Child->GetAttachParent())
						{
							auto* Parent = Child->GetAttachParent();
							if (Selected.contains(Actor) != Selected.contains(Parent->GetOwner()))
								Attachments.push_back({Child, Parent, Child->GetRelativeTransform()});
						}
				AfterCamera = BeforeCamera;
				if (bRemove && Selected.contains(BeforeCamera.Get())) AfterCamera = nullptr;
				if (!AfterCamera)
					for (AActor* Actor : All)
						if (bRemove ? !Selected.contains(Actor) : Selected.contains(Actor))
							if (auto* Camera = Cast<ACameraActor>(Actor)) { AfterCamera = Camera; break; }
				RetainActors();
			}
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto GetAllocatedSize() const -> size_t override
			{
				size_t Bytes = Description.capacity() + States.capacity() * sizeof(FActorState)
					+ Attachments.capacity() * sizeof(FAttachmentState) + Retained.capacity() * sizeof(TObjectPtr<DObject>);
				for (const auto& State : States) Bytes += State.Registered.capacity() * sizeof(TObjectPtr<DActorComponent>);
				return Bytes;
			}
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (DObject* Object : {static_cast<DObject*>(Level.Get()), static_cast<DObject*>(BeforeCamera.Get()), static_cast<DObject*>(AfterCamera.Get())})
					if (Object) Collector.AddReferencedObject(Object);
				for (const auto& Ref : Retained) { DObject* Object = Ref.Get(); if (Object) Collector.AddReferencedObject(Object); }
				for (const auto& Link : Attachments)
					for (DObject* Object : {static_cast<DObject*>(Link.Child.Get()), static_cast<DObject*>(Link.Parent.Get())})
						if (Object) Collector.AddReferencedObject(Object);
			}
			auto Replay(ETransactionOperation Operation) -> FTransactionCustomResult override
			{
				const bool bAfter = Operation != ETransactionOperation::Undo;
				const bool bPresent = bAfter != bRemove;
				const auto Reject = [](ETransactionCustomError Code) -> FTransactionCustomResult { return {{.Code = Code}}; };
				if (!IsValid(Level.Get()) || !CanEdit(*Level)) return Reject(ETransactionCustomError::TargetUnavailable);
				if (Level->GetPrimaryCameraActor() != (bAfter ? BeforeCamera : AfterCamera).Get()) return Reject(ETransactionCustomError::ActorChanged);
				auto* DesiredCamera = (bAfter ? AfterCamera : BeforeCamera).Get();
				if (DesiredCamera && (!IsValid(DesiredCamera) || (!Level->ContainsActor(DesiredCamera)
					&& (!bPresent || std::ranges::none_of(States, [DesiredCamera](const auto& State) { return State.Actor.Get() == DesiredCamera; }))))) return Reject(ETransactionCustomError::ActorMembership);
				for (const auto& State : States)
				{
					auto* Actor = State.Actor.Get();
					if (!IsValid(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || Actor->GetOuter() != Level.Get()
						|| Level->ContainsActor(Actor) == bPresent) return Reject(ETransactionCustomError::ActorMembership);
					if (Actor->GetFName() != State.Name) return Reject(ETransactionCustomError::ActorChanged);
					if (bPresent)
						if (auto* Collision = Level->FindActorByName(Actor->GetFName()); Collision && Collision != Actor) return Reject(ETransactionCustomError::ActorNameCollision);
					for (const auto& Component : Actor->GetComponents())
						if (!IsValid(Component.Get()) || Component->IsBeingDestroyed() || Component->HasBegunPlay()) return Reject(ETransactionCustomError::TargetUnavailable);
				}
				for (const auto& Link : Attachments)
				{
					if (!IsValid(Link.Child.Get()) || !IsValid(Link.Parent.Get()) || Link.Child->IsBeingDestroyed() || Link.Parent->IsBeingDestroyed()) return Reject(ETransactionCustomError::ParentInvalid);
					for (DSceneComponent* Scene : {Link.Child.Get(), Link.Parent.Get()})
					{
						auto* Owner = Scene->GetOwner();
						if (!IsValid(Owner) || Owner->GetOuter() != Level.Get() || !Owner->OwnsComponent(Scene)
							|| (!Level->ContainsActor(Owner) && std::ranges::none_of(States, [Owner](const auto& State) { return State.Actor.Get() == Owner; }))) return Reject(ETransactionCustomError::ParentInvalid);
					}
					if (Link.Child->GetAttachParent() != (bPresent ? nullptr : Link.Parent.Get())) return Reject(ETransactionCustomError::ParentMismatch);
					if (bPresent && (Link.Parent.Get() == Link.Child.Get() || Link.Parent->IsAttachedTo(Link.Child.Get()))) return Reject(ETransactionCustomError::ParentCycle);
				}
				if (bPresent)
				{
					std::unordered_map<DSceneComponent*, DSceneComponent*> Parents;
					for (const auto& Link : Attachments) Parents.emplace(Link.Child.Get(), Link.Parent.Get());
					for (const auto& Link : Attachments)
					{
						std::unordered_set<DSceneComponent*> Visited;
						for (auto* Parent = Link.Parent.Get(); Parent;)
						{
							if (Parent == Link.Child.Get() || !Visited.insert(Parent).second) return Reject(ETransactionCustomError::ParentCycle);
							const auto It = Parents.find(Parent);
							Parent = It == Parents.end() ? Parent->GetAttachParent() : It->second;
						}
					}
				}
				if (!bPresent)
				{
					RetainActors();
					for (const auto& Link : Attachments) verify(Link.Child->DetachFromComponent(EDetachmentTransformRule::KeepWorld));
					for (auto It = States.rbegin(); It != States.rend(); ++It)
					{
						It->Registered.clear();
						for (const auto& Component : It->Actor->GetComponents()) if (Component->IsRegistered()) It->Registered.push_back(Component);
						verify(Level->SetActorPresence(It->Actor.Get(), false));
						It->Actor->SetTransient(true);
					}
				}
				else
				{
					for (const auto& State : States)
					{
						State.Actor->SetTransient(State.bTransient);
						verify(Level->SetActorPresence(State.Actor.Get(), true, State.Index, false));
						if (!State.bTick) State.Actor->GetPrimaryActorTick().UnregisterTickFunction();
					}
					for (const auto& Link : Attachments)
					{
						verify(Link.Child->AttachToComponent(Link.Parent.Get(), EAttachmentTransformRule::KeepRelative));
						Link.Child->SetRelativeTransform(Link.Relative);
					}
					for (const auto& State : States)
						for (const auto& Component : State.Registered) if (IsValid(Component.Get()) && State.Actor->OwnsComponent(Component.Get())) Component->RegisterComponent();
				}
				verify(Level->SetPrimaryCameraActor((bAfter ? AfterCamera : BeforeCamera).Get()));
				return {};
			}
		private:
			auto RetainTree(DObject* Object) -> void
			{
				Retained.emplace_back(Object);
				for (DObject* Child : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::LiveOnly)) RetainTree(Child);
			}
			auto RetainActors() -> void { Retained.clear(); for (const auto& State : States) RetainTree(State.Actor.Get()); }
			TObjectPtr<DLevel> Level;
			std::string Description;
			bool bRemove;
			TObjectPtr<ACameraActor> BeforeCamera, AfterCamera;
			std::vector<FActorState> States;
			std::vector<FAttachmentState> Attachments;
			std::vector<TObjectPtr<DObject>> Retained;
			std::array<DPackage*, 1> Packages{};
		};

		class FActorRenameChange final : public ITransactionCustomChange
		{
		public:
			FActorRenameChange(DLevel& InLevel, AActor& InActor, FName InAfter) : Level(&InLevel), Actor(&InActor), Before(InActor.GetFName()), After(InAfter), Packages{InLevel.GetPackage()} {}
			auto GetDescription() const -> std::string_view override { return "Rename actor"; }
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{ for (DObject* Object : {static_cast<DObject*>(Level.Get()), static_cast<DObject*>(Actor.Get())}) if (Object) Collector.AddReferencedObject(Object); }
			auto Replay(ETransactionOperation Operation) -> FTransactionCustomResult override
			{
				if (!IsValid(Level.Get()) || !CanEdit(*Level) || !IsValid(Actor.Get()) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Level->ContainsActor(Actor.Get())) return {{.Code = ETransactionCustomError::ActorMembership}};
				const bool bAfter = Operation != ETransactionOperation::Undo;
				const FName Name = bAfter ? After : Before;
				if (Actor->GetFName() != (bAfter ? Before : After)) return {{.Code = ETransactionCustomError::ActorChanged}};
				for (DObject* Object : GDObjectArray.GetObjectsWithOuter(Level.Get(), EObjectQueryScope::LiveOnly))
					if (Object != Actor.Get() && Object->GetFName() == Name) return {{.Code = ETransactionCustomError::ActorNameCollision}};
				verify(Level->RenameActor(Actor.Get(), Name));
				return {};
			}
		private:
			TObjectPtr<DLevel> Level;
			TObjectPtr<AActor> Actor;
			FName Before, After;
			std::array<DPackage*, 1> Packages;
		};
		auto Execute(std::unique_ptr<ITransactionCustomChange> Change, std::vector<TObjectPtr<AActor>> Actors, DTransactor& Transactions) -> FActorMutationResult
		{
			const auto Result = Transactions.Execute(std::move(Change));
			if (!Result)
				for (const auto& Actor : Actors)
					if (IsValid(Actor.Get()))
						if (auto* Level = Cast<DLevel>(Actor->GetOuter()); Level && !Level->ContainsActor(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get());
			return Result ? FActorMutationResult{std::move(Actors), {}} : FActorMutationResult{{}, FormatTransactorResult(Result)};
		}
	}

	auto FActorMutations::MakePresenceChange(DLevel& Level, std::span<AActor* const> Actors, bool bRemove, std::string Description) -> std::unique_ptr<ITransactionCustomChange>
	{ return std::make_unique<FActorPresenceChange>(Level, Actors, bRemove, std::move(Description)); }

	auto FActorMutations::Add(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
		DTransactor* Transactions, bool bReadOnly, std::function<void(AActor&)> Configure) -> FActorMutationResult
	{
		if (!CanEdit(Level, bReadOnly) || !Transactions || !CanConstructObjectOfClass(Class, AActor::StaticClass())) return {{}, "Invalid actor creation target."};
		AActor* Actor = nullptr;
		{
			FScopedPackageDirtySuppression SuppressPreparation;
			Actor = NewObject<AActor>(Class, &Level, UniqueName(Level, Name.IsNone() ? FName(Class->GetDefaultObjectName()) : Name));
			if (Actor && Actor->RequestNativeReconstruction())
			{
				Actor->SetActorTransform(Transform);
				if (Configure) Configure(*Actor);
			}
			else { if (Actor) MarkObjectHierarchyAsGarbage(Actor); return {{}, "Actor construction failed."}; }
		}
		auto Change = MakePresenceChange(Level, {&Actor, 1}, false, "Add actor");
		Actor->SetTransient(true);
		return Execute(std::move(Change), {Actor}, *Transactions);
	}
	auto FActorMutations::Remove(DLevel& Level, std::span<AActor* const> Actors, DTransactor* Transactions, bool bReadOnly) -> FActorMutationResult
	{
		if (!CanEdit(Level, bReadOnly) || !Transactions || Actors.empty()) return {{}, "Invalid actor deletion target."};
		std::unordered_set<AActor*> Unique;
		for (AActor* Actor : Actors)
			if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Unique.insert(Actor).second) return {{}, "Invalid actor deletion selection."};
		return Execute(MakePresenceChange(Level, Actors, true, "Delete actors"), {Actors.begin(), Actors.end()}, *Transactions);
	}
	auto FActorMutations::Duplicate(DLevel& Level, std::span<AActor* const> Actors, DTransactor* Transactions, bool bReadOnly) -> FActorMutationResult
	{
		if (!CanEdit(Level, bReadOnly) || !Transactions || Actors.empty()) return {{}, "Invalid actor duplication target."};
		std::unordered_set<AActor*> Unique;
		for (AActor* Actor : Actors)
			if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Unique.insert(Actor).second) return {{}, "Invalid actor duplication selection."};
		std::vector<TObjectPtr<AActor>> Copies;
		std::vector<AActor*> Raw;
		std::unique_ptr<ITransactionCustomChange> Change;
		{
			FScopedPackageDirtySuppression SuppressPreparation;
			std::unordered_map<DObject*, DObject*> Duplicates;
			for (AActor* Source : Actors)
			{
				std::unordered_map<DObject*, DObject*> Graph;
				auto Copy = DuplicateObject(Source, &Level, UniqueName(Level, Source->GetFName()), &Graph);
				if (!Copy) { for (const auto& Actor : Copies) MarkObjectHierarchyAsGarbage(Actor.Get()); return {{}, ToString(Copy.error())}; }
				Copies.push_back(*Copy);
				Raw.push_back(*Copy);
				Duplicates.insert(Graph.begin(), Graph.end());
				// Duplication remaps the complete owned graph and runs PostLoad before publication.
			}
			// Reconnect references between selected graphs as well as within each Outer tree.
			for (const auto& [Source, Copy] : Duplicates)
			{
				if (!IsValid(Copy)) continue;
				if (const auto Result = CopyEditableObjectProperties(Source, Copy, Duplicates); !Result)
				{ for (const auto& Actor : Copies) MarkObjectHierarchyAsGarbage(Actor.Get()); return {{}, ToString(Result.error())}; }
				if (auto* Scene = Cast<DSceneComponent>(Copy))
				{
					auto* Parent = static_cast<DSceneComponent*>(Source)->GetAttachParent();
					if (auto It = Duplicates.find(Parent); It != Duplicates.end()) Parent = static_cast<DSceneComponent*>(It->second);
					if (Parent) verify(Scene->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative));
					else { if (Scene->GetAttachParent()) Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative); Scene->UpdateComponentToWorld(); }
				}
			}
			// Rebuild derived data after editable cross-graph references are remapped.
			for (AActor* Actor : Raw)
				for (const auto& Component : Actor->GetComponentsSnapshot())
					if (IsValid(Component.Get()) && Component->GetCreationMethod() != EComponentCreationMethod::Generated) Component->PostLoad();
			Change = MakePresenceChange(Level, Raw, false, "Duplicate actors");
			// Boundary links are published by replay, rather than left visible after rejection.
			const std::unordered_set<AActor*> CopiedOwners(Raw.begin(), Raw.end());
			for (AActor* Actor : Raw)
			{
				for (const auto& Component : Actor->GetComponents())
					if (auto* Scene = Cast<DSceneComponent>(Component.Get()); Scene && Scene->GetAttachParent() && !CopiedOwners.contains(Scene->GetAttachParent()->GetOwner()))
						Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
				Actor->SetTransient(true);
			}
		}
		return Execute(std::move(Change), std::move(Copies), *Transactions);
	}
	auto FActorMutations::Rename(DLevel& Level, AActor* Actor, FName Name, DTransactor* Transactions, bool bReadOnly) -> FActorMutationResult
	{
		if (!CanEdit(Level, bReadOnly) || !Transactions || !IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsTemplateObject() || Name.IsNone()) return {{}, "Invalid actor rename target."};
		Name = UniqueName(Level, Name, Actor);
		if (Actor->GetFName() == Name) return {{Actor}, {}};
		return Execute(std::make_unique<FActorRenameChange>(Level, *Actor, Name), {Actor}, *Transactions);
	}
}
