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
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{
				std::vector<DObject*> Objects{Level.Get()};
				for (const auto& State : States)
				{
					Objects.push_back(State.Actor.Get());
					for (const auto& Component : State.Registered) Objects.push_back(Component.Get());
				}
				for (const auto& Link : Attachments)
				{
					Objects.push_back(Link.Child.Get());
					Objects.push_back(Link.Parent.Get());
				}
				return Objects;
			}

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
				const bool bHistorical = Level->IsTransactionRestorePending();
				if (!bHistorical && Level->GetPrimaryCameraActor() != (bAfter ? BeforeCamera : AfterCamera).Get()) return Reject(EActorChangeError::ActorChanged);
				auto* DesiredCamera = (bAfter ? AfterCamera : BeforeCamera).Get();
				if (!bHistorical && DesiredCamera && (!IsValid(DesiredCamera) || (!Level->ContainsActor(DesiredCamera)
					&& (!bPresent || std::ranges::none_of(States, [DesiredCamera](const auto& State) { return State.Actor.Get() == DesiredCamera; }))))) return Reject(EActorChangeError::ActorMembership);
				for (const auto& State : States)
				{
					auto* Actor = State.Actor.Get();
					if (!IsValid(Actor) || Actor->IsBeingDestroyed() || Actor->HasBegunPlay() || Actor->GetOuter() != Level.Get()
						|| (Level->ContainsActor(Actor) == bPresent && !Level->IsActorMembershipChangedDuringUndo(Actor))) return Reject(EActorChangeError::ActorMembership);
					if (Actor->GetFName() != State.Name) return Reject(EActorChangeError::ActorChanged);
					if (bPresent)
						if (!Level->IsActorNameAvailable(Actor->GetFName(), Actor)) return Reject(EActorChangeError::ActorNameCollision);
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
				if (GUndo && !bHistorical)
				{
					// One serialized owner for Level membership and camera fields. The
					// command retains only native publication and local compensation.
					GUndo->SaveObject(Level.Get());
					if (!GUndo->ContainsObject(Level.Get())) return Reject(EActorChangeError::MemberUnavailable);
				}
				if (!bPresent)
				{
					RetainActors();
					for (const auto& Link : Attachments) verify(Link.Child->DetachFromComponent(EDetachmentTransformRule::KeepWorld));
					for (auto It = States.rbegin(); It != States.rend(); ++It)
					{
						if (!CanEditActorLevel(Level.Get()) || !IsValid(It->Actor.Get())
							|| It->Actor->IsBeingDestroyed() || It->Actor->HasBegunPlay()
							|| (!Level->ContainsActor(It->Actor.Get()) && !Level->IsActorMembershipChangedDuringUndo(It->Actor.Get()))) return Interrupted();
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
				if (!bHistorical && !Level->SetPrimaryCameraActor((bAfter ? AfterCamera : BeforeCamera).Get())) return Interrupted();
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

		auto Execute(std::unique_ptr<FActorCommandChange> Change, std::vector<TObjectPtr<AActor>> Actors, DTransactor& Transactions) -> bool
		{
			const auto Result = ::Durin::Editor::ApplyEditorChange(Transactions, std::move(Change));
			if (!Result && Result.Code != ETransactorResultCode::RecoveryRequired)
				for (const auto& Actor : Actors)
					if (IsValid(Actor.Get()))
						if (auto* Level = Cast<DLevel>(Actor->GetOuter()); Level && !Level->ContainsActor(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get());
			if (!Result) return false;
			return true;
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
		const FActorEditContext& Context, std::function<void(AActor&)> Configure, ESpawnActorNameMode NameMode, const std::string& Label) -> AActor*
	{
		auto Admission = Admit(Context, &Level);
		if (!Admission) return nullptr;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> AActor*
		{
			auto* Transactions = Admission;
			if (!CanConstructObjectOfClass(Class, AActor::StaticClass())) return nullptr;
			DObjectForceRegistration(Class);
			if (Label.find('\0') != std::string::npos) return nullptr;
			const FName Requested = Name.IsNone()
				? FName(Class->GetDefaultObjectName().empty() ? Class->GetName() : Class->GetDefaultObjectName()) : Name;
			Name = Level.ResolveActorName(Requested, Name.IsNone() ? ESpawnActorNameMode::Requested : NameMode);
			if (Name.IsNone()) return nullptr;
			AActor* Actor = nullptr;
			{
				FScopedPackageDirtySuppression SuppressPreparation;
				Actor = NewObject<AActor>(Class, &Level, Name, EObjectFlags::Transactional);
				if (Actor) Actor->SetInitialActorLabel(Level.MakeUniqueActorLabel(Label.empty() ? Requested.ToString() : Label));
				if (Actor && Actor->RerunConstructionScripts())
				{
					Actor->SetActorTransform(Transform);
					if (Configure)
					{
						try { Configure(*Actor); }
						catch (...) { MarkObjectHierarchyAsGarbage(Actor); return nullptr; }
					}
					if (auto Revalidated = Admit(Context, &Level, nullptr, true); !Revalidated)
					{ if (IsValid(Actor)) MarkObjectHierarchyAsGarbage(Actor); return nullptr; }
					if (!IsValid(Actor)) return nullptr;
				}
				else { if (Actor) MarkObjectHierarchyAsGarbage(Actor); return nullptr; }
			}
			auto Change = MakeActorPresenceChange(Level, {&Actor, 1}, false, "Add actor");
			Actor->SetTransient(true);
			if (auto Applied = Execute(std::move(Change), {Actor}, *Transactions); !Applied) return nullptr;
			return Actor;

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
			return Execute(MakeActorPresenceChange(Level, Actors, true, "Delete actors"), {Actors.begin(), Actors.end()}, *Transactions);

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
			std::unique_ptr<FActorCommandChange> Change;
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
				std::vector<FAttachmentState> PreparedAttachments;
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
			{ for (const auto& Actor : Copies) if (IsValid(Actor.Get())) MarkObjectHierarchyAsGarbage(Actor.Get()); return {}; }
			if (auto Applied = Execute(std::move(Change), Copies, *Transactions); !Applied) return {};
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
			FScopedTransaction Recording(Transactions, {"ActorLabel", "Set actor label", Actor});
			if (!Recording.IsOutstanding() || !RecordObjectForEdit(*Transactions, *Actor)) return false;
			if (!Actor->SetActorLabel(Resolved)) return false;
			(void)Recording.End();
			return Transactions->GetLastResult().Code != ETransactorResultCode::Failed
				&& Transactions->GetLastResult().Code != ETransactorResultCode::RecoveryRequired;
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
			FScopedTransaction Recording(Transactions, {"ActorRename", "Rename actor", Actor});
			if (!Recording.IsOutstanding() || !RecordObjectForEdit(*Transactions, *Actor)) return false;
			if (!Level.RenameActor(Actor, Name, ESpawnActorNameMode::RequiredReturnNull)) return false;
			(void)Recording.End();
			return Transactions->GetLastResult().Code != ETransactorResultCode::Failed
				&& Transactions->GetLastResult().Code != ETransactorResultCode::RecoveryRequired;

		});
	}
}
