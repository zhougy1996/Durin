#include "Actors/EditorActorSubsystem.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorChange.h"
#include "Actors/ActorEditingScope.h"
#include "Actors/ActorEditingValidation.h"

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

namespace Durin
{
	using namespace Editor;
	namespace
	{
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

		class FActorPresenceChange final : public FActorCommandChange
		{
		public:
			FActorPresenceChange(DLevel& InLevel, std::span<AActor* const> InActors, bool bInRemove, std::string InDescription,
				std::span<const FAttachmentState> PreparedAttachments = {})
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
				Attachments.insert(Attachments.end(), PreparedAttachments.begin(), PreparedAttachments.end());
				AfterCamera = BeforeCamera;
				if (bRemove && Selected.contains(BeforeCamera.Get())) AfterCamera = nullptr;
				if (!AfterCamera)
					for (AActor* Actor : All)
						if (bRemove ? !Selected.contains(Actor) : Selected.contains(Actor))
							if (auto* Camera = Cast<ACameraActor>(Actor)) { AfterCamera = Camera; break; }
				RetainActors();
			}
			auto GetDescription() const -> std::string_view override { return Description; }
			auto GetOwningModule() const -> std::string_view override { return "DurinEd"; }
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
			auto Replay(ETransactionOperation Operation) -> FActorChangeResult override
			{
				FScopedPackageDirtySuppression SuppressReplay;
				const bool bAfter = Operation != ETransactionOperation::Undo;
				const bool bPresent = bAfter != bRemove;
				const auto Reject = [](EActorChangeError Code) -> FActorChangeResult { return {{.Code = Code}}; };
				const auto Interrupted = []() -> FActorChangeResult {
					return {{.Code = EActorChangeError::TargetUnavailable,
						.CleanupCause = std::make_shared<FActorChangeError>(FActorChangeError{.Code = EActorChangeError::RollbackIncomplete})}};
				};
				if (!CanEditActorLevel(Level.Get())) return Reject(EActorChangeError::TargetUnavailable);
				if (Level->GetPrimaryCameraActor() != (bAfter ? BeforeCamera : AfterCamera).Get()) return Reject(EActorChangeError::ActorChanged);
				auto* DesiredCamera = (bAfter ? AfterCamera : BeforeCamera).Get();
				if (DesiredCamera && (!IsValid(DesiredCamera) || (!Level->ContainsActor(DesiredCamera)
					&& (!bPresent || std::ranges::none_of(States, [DesiredCamera](const auto& State) { return State.Actor.Get() == DesiredCamera; }))))) return Reject(EActorChangeError::ActorMembership);
				for (const auto& State : States)
				{
					auto* Actor = State.Actor.Get();
					if (!IsValid(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || Actor->GetOuter() != Level.Get()
						|| Level->ContainsActor(Actor) == bPresent) return Reject(EActorChangeError::ActorMembership);
					if (Actor->GetFName() != State.Name) return Reject(EActorChangeError::ActorChanged);
					if (bPresent)
						if (auto* Collision = Level->FindActorByName(Actor->GetFName()); Collision && Collision != Actor) return Reject(EActorChangeError::ActorNameCollision);
					for (const auto& Component : Actor->GetComponents())
						if (!IsValid(Component.Get()) || Component->IsBeingDestroyed() || Component->HasBegunPlay()) return Reject(EActorChangeError::TargetUnavailable);
				}
				for (const auto& Link : Attachments)
				{
					if (!IsValid(Link.Child.Get()) || !IsValid(Link.Parent.Get()) || Link.Child->IsBeingDestroyed() || Link.Parent->IsBeingDestroyed()) return Reject(EActorChangeError::ParentInvalid);
					for (DSceneComponent* Scene : {Link.Child.Get(), Link.Parent.Get()})
					{
						auto* Owner = Scene->GetOwner();
						if (!IsValid(Owner) || Owner->GetOuter() != Level.Get() || !Owner->OwnsComponent(Scene)
							|| (!Level->ContainsActor(Owner) && std::ranges::none_of(States, [Owner](const auto& State) { return State.Actor.Get() == Owner; }))) return Reject(EActorChangeError::ParentInvalid);
					}
					if (Link.Child->GetAttachParent() != (bPresent ? nullptr : Link.Parent.Get())) return Reject(EActorChangeError::ParentMismatch);
					if (bPresent && (Link.Parent.Get() == Link.Child.Get() || Link.Parent->IsAttachedTo(Link.Child.Get()))) return Reject(EActorChangeError::ParentCycle);
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
							if (Parent == Link.Child.Get() || !Visited.insert(Parent).second) return Reject(EActorChangeError::ParentCycle);
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
						if (!CanEditActorOwner(It->Actor.Get())) return Interrupted();
						It->Registered.clear();
						for (const auto& Component : It->Actor->GetComponents()) if (Component->IsRegistered()) It->Registered.push_back(Component);
						if (!Level->SetActorPresence(It->Actor.Get(), false)
							|| !CanEditActorLevel(Level.Get()) || !IsValid(It->Actor.Get()) || It->Actor->IsBeingDestroyed()
							|| Level->ContainsActor(It->Actor.Get()))
							return Interrupted();
						It->Actor->SetTransient(true);
					}
				}
				else
				{
					for (const auto& State : States)
					{
						if (!IsValid(State.Actor.Get()) || State.Actor->IsBeingDestroyed()) return Interrupted();
						State.Actor->SetTransient(State.bTransient);
						if (!Level->SetActorPresence(State.Actor.Get(), true, State.Index, false)
							|| !CanEditActorOwner(State.Actor.Get())) return Interrupted();
						if (!State.bTick) State.Actor->GetPrimaryActorTick().UnregisterTickFunction();
					}
					for (const auto& Link : Attachments)
					{
						verify(Link.Child->AttachToComponent(Link.Parent.Get(), EAttachmentTransformRule::KeepRelative));
						Link.Child->SetRelativeTransform(Link.Relative);
					}
					for (const auto& State : States)
						for (const auto& Component : State.Registered)
						{
							if (IsValid(Component.Get()) && State.Actor->OwnsComponent(Component.Get())) Component->RegisterComponent();
							if (!CanEditActorOwner(State.Actor.Get())) return Interrupted();
						}
				}
				RetainActors();
				if (!Level->SetPrimaryCameraActor((bAfter ? AfterCamera : BeforeCamera).Get())) return Interrupted();
				return {};
			}
		private:
			auto RetainTree(DObject* Object) -> void
			{
				if (!IsValid(Object)) return;
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

		class FActorRenameChange final : public FActorCommandChange
		{
		public:
			FActorRenameChange(DLevel& InLevel, AActor& InActor, FName InAfter) : Level(&InLevel), Actor(&InActor), Before(InActor.GetFName()), After(InAfter), Packages{InLevel.GetPackage()} {}
			auto GetDescription() const -> std::string_view override { return "Rename actor"; }
			auto GetOwningModule() const -> std::string_view override { return "DurinEd"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{ for (DObject* Object : {static_cast<DObject*>(Level.Get()), static_cast<DObject*>(Actor.Get())}) if (Object) Collector.AddReferencedObject(Object); }
			auto Replay(ETransactionOperation Operation) -> FActorChangeResult override
			{
				FScopedPackageDirtySuppression SuppressReplay;
				if (!CanEditActorLevel(Level.Get()) || !IsValid(Actor.Get()) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Level->ContainsActor(Actor.Get())) return {{.Code = EActorChangeError::ActorMembership}};
				const bool bAfter = Operation != ETransactionOperation::Undo;
				const FName Name = bAfter ? After : Before;
				if (Actor->GetFName() != (bAfter ? Before : After)) return {{.Code = EActorChangeError::ActorChanged}};
				for (DObject* Object : GDObjectArray.GetObjectsWithOuter(Level.Get(), EObjectQueryScope::LiveOnly))
					if (Object != Actor.Get() && Object->GetFName() == Name) return {{.Code = EActorChangeError::ActorNameCollision}};
				verify(Level->RenameActor(Actor.Get(), Name));
				return {};
			}
		private:
			TObjectPtr<DLevel> Level;
			TObjectPtr<AActor> Actor;
			FName Before, After;
			std::array<DPackage*, 1> Packages;
		};
		auto Execute(std::unique_ptr<FActorCommandChange> Change, std::vector<TObjectPtr<AActor>> Actors, DTransactor& Transactions) -> FActorEditResult
		{
			const auto Result = ::Durin::Editor::ApplyEditorChange(Transactions, std::move(Change));
			if (!Result && Result.Code != ETransactorResultCode::RecoveryRequired)
				for (const auto& Actor : Actors)
					if (IsValid(Actor.Get()))
						if (auto* Level = Cast<DLevel>(Actor->GetOuter()); Level && !Level->ContainsActor(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get());
			return Result ? FActorEditResult{std::move(Actors), {}, EActorEditStatus::Changed, EActorEditError::None, Result} : FActorEditResult{{}, FormatTransactorResult(Result), EActorEditStatus::Failed, EActorEditError::HistoryRejected, Result};
		}
	}

	auto Editor::MakeActorPresenceChange(DLevel& Level, std::span<AActor* const> Actors, bool bRemove, std::string Description) -> std::unique_ptr<FActorCommandChange>
	{
		if (!CanEditActorLevel(&Level) || Actors.empty()) return {};
		std::unordered_set<AActor*> Unique;
		for (auto* Actor : Actors)
			if (!IsValid(Actor) || Actor->GetOuter() != &Level || Actor->IsBeingDestroyed() || Actor->HasBegunPlay()
				|| Level.ContainsActor(Actor) != bRemove || !Unique.insert(Actor).second) return {};
		return std::make_unique<FActorPresenceChange>(Level, Actors, bRemove, std::move(Description));
	}

	auto DEditorActorSubsystem::SpawnActor(DLevel& Level, DClass* Class, FName Name, const FTransform& Transform,
		const FActorEditContext& Context, std::function<void(AActor&)> Configure) -> FActorEditResult
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return Admission.error();
		return RunActorEdit(GetEditor(), bEditing, [&]() -> FActorEditResult
		{
			auto* Transactions = *Admission;
			if (!CanConstructObjectOfClass(Class, AActor::StaticClass())) return {{}, "Invalid actor creation target."};
			AActor* Actor = nullptr;
			{
				FScopedPackageDirtySuppression SuppressPreparation;
				Actor = NewObject<AActor>(Class, &Level, UniqueName(Level, Name.IsNone() ? FName(Class->GetDefaultObjectName()) : Name), EObjectFlags::Transactional);
				if (Actor && Actor->RequestNativeReconstruction())
				{
					Actor->SetActorTransform(Transform);
					if (Configure)
					{
						try { Configure(*Actor); }
						catch (...) { MarkObjectHierarchyAsGarbage(Actor); return {{}, "Actor preparation failed."}; }
					}
					if (auto Revalidated = Admit(Context, &Level, nullptr, true); !Revalidated)
					{ if (IsValid(Actor)) MarkObjectHierarchyAsGarbage(Actor); return Revalidated.error(); }
					if (!IsValid(Actor)) return {{}, "The prepared actor is unavailable."};
				}
				else { if (Actor) MarkObjectHierarchyAsGarbage(Actor); return {{}, "Actor construction failed."}; }
			}
			auto Change = MakeActorPresenceChange(Level, {&Actor, 1}, false, "Add actor");
			Actor->SetTransient(true);
			return Execute(std::move(Change), {Actor}, *Transactions);

		});
	}
	auto DEditorActorSubsystem::DestroyActors(DLevel& Level, std::span<AActor* const> Actors, const FActorEditContext& Context) -> FActorEditResult
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return Admission.error();
		return RunActorEdit(GetEditor(), bEditing, [&]() -> FActorEditResult
		{
			auto* Transactions = *Admission;
			if (Actors.empty()) return {{}, "Invalid actor deletion target."};
			std::unordered_set<AActor*> Unique;
			for (AActor* Actor : Actors)
				if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Unique.insert(Actor).second) return {{}, "Invalid actor deletion selection."};
			return Execute(MakeActorPresenceChange(Level, Actors, true, "Delete actors"), {Actors.begin(), Actors.end()}, *Transactions);

		});
	}
	auto DEditorActorSubsystem::DuplicateActors(DLevel& Level, std::span<AActor* const> Actors, const FActorEditContext& Context) -> FActorEditResult
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return Admission.error();
		return RunActorEdit(GetEditor(), bEditing, [&]() -> FActorEditResult
		{
			auto* Transactions = *Admission;
			if (Actors.empty()) return {{}, "Invalid actor duplication target."};
			std::unordered_set<AActor*> Unique;
			for (AActor* Actor : Actors)
				if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || !Unique.insert(Actor).second) return {{}, "Invalid actor duplication selection."};
			std::vector<TObjectPtr<AActor>> Copies;
			std::vector<AActor*> Raw;
			std::unique_ptr<FActorCommandChange> Change;
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
				const std::unordered_set<AActor*> CopiedOwners(Raw.begin(), Raw.end());
				std::vector<FAttachmentState> PreparedAttachments;
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
				Change = std::make_unique<FActorPresenceChange>(Level, Raw, false, "Duplicate actors", PreparedAttachments);
				// External parent child lists are changed only by successful replay.
				for (AActor* Actor : Raw)
				{
					for (const auto& Component : Actor->GetComponents())
						if (auto* Scene = Cast<DSceneComponent>(Component.Get()); Scene && Scene->GetAttachParent() && !CopiedOwners.contains(Scene->GetAttachParent()->GetOwner()))
							Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
					Actor->SetTransient(true);
				}
			}
			if (auto Revalidated = Admit(Context, &Level, nullptr, true); !Revalidated)
			{ for (const auto& Actor : Copies) if (IsValid(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get()); return Revalidated.error(); }
			return Execute(std::move(Change), std::move(Copies), *Transactions);

		});
	}
	auto DEditorActorSubsystem::RenameActor(DLevel& Level, AActor* Actor, FName Name, const FActorEditContext& Context) -> FActorEditResult
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return Admission.error();
		return RunActorEdit(GetEditor(), bEditing, [&]() -> FActorEditResult
		{
			auto* Transactions = *Admission;
			if (!IsValid(Actor) || !Level.ContainsActor(Actor) || Actor->IsTemplateObject() || Name.IsNone()) return {{}, "Invalid actor rename target."};
			Name = UniqueName(Level, Name, Actor);
			if (Actor->GetFName() == Name) return {{Actor}, {}, EActorEditStatus::NoOp, EActorEditError::None};
			return Execute(std::make_unique<FActorRenameChange>(Level, *Actor, Name), {Actor}, *Transactions);

		});
	}
}
