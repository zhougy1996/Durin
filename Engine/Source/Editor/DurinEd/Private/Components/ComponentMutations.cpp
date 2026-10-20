#include "Actors/EditorActorSubsystem.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorChange.h"
#include "Actors/ActorEditingScope.h"
#include "Actors/ActorEditingValidation.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"

namespace Durin
{
	using namespace Editor;
	namespace
	{
		struct FChildAttachment
		{
			TObjectPtr<DSceneComponent> Child;
			FTransform Relative;
		};

		// Keeps an absent component alive, rather than resurrecting a garbage object or
		// replacing its identity and invalidating older property-history records.
		class FComponentMembershipChange final : public FActorCommandChange
		{
		public:
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{
				std::vector<DObject*> Objects{Owner.Get(), Component.Get(), Parent.Get()};
				for (const auto& Entry : Children) Objects.push_back(Entry.Child.Get());
				return Objects;
			}

			FComponentMembershipChange(AActor& InOwner, DActorComponent& InComponent, bool bInRemove)
				: Owner(&InOwner), Component(&InComponent), bRemove(bInRemove)
			{
				Packages[0] = Owner->GetPackage();
				bRegistered = bRemove ? Component->IsRegistered() : true;
				bTransient = Component->HasAnyObjectFlags(EObjectFlags::Transient);
				bRoot = Owner->GetRootComponent() == &InComponent;
				const auto& Instances = Owner->GetInstanceComponents();
				InstanceIndex = 0;
				while (InstanceIndex < Instances.size() && Instances[InstanceIndex].Get() != &InComponent) ++InstanceIndex;
				if (auto* Scene = Cast<DSceneComponent>(&InComponent))
				{
					Parent = Scene->GetAttachParent();
					Relative = Scene->GetRelativeTransform();
					for (const auto& Child : Scene->GetAttachChildren())
						if (Child && Child->GetAttachParent() == Scene)
							Children.push_back({Child, Child->GetRelativeTransform()});
				}
				RetainTree(&InComponent);
			}

			auto ConfigureAddition(DSceneComponent* InParent, bool bInRoot, const FTransform& InRelative,
				bool bInTransient = false, bool bInDuplicate = false) -> void
			{
				Parent = InParent;
				bRoot = bInRoot;
				Relative = InRelative;
				bTransient = bInTransient;
				bDuplicate = bInDuplicate;
			}
			auto GetDescription() const -> std::string_view override { return bRemove ? "Delete component" : bDuplicate ? "Duplicate component" : "Add component"; }
			auto GetOwningModule() const -> std::string_view override { return "DurinEd"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto GetAllocatedSize() const -> size_t override
			{ return Children.capacity() * sizeof(FChildAttachment) + Retained.capacity() * sizeof(TObjectPtr<DObject>); }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (DObject* Object : {static_cast<DObject*>(Owner.Get()), static_cast<DObject*>(Parent.Get())})
					if (Object) Collector.AddReferencedObject(Object);
				for (const auto& Object : Retained)
				{
					DObject* Value = Object.Get();
					if (Value) Collector.AddReferencedObject(Value);
				}
				for (const auto& Entry : Children)
				{
					DObject* Value = Entry.Child.Get();
					if (Value) Collector.AddReferencedObject(Value);
				}
			}
			auto Replay(ETransactionOperation Operation) -> FActorChangeResult override
			{
				FScopedPackageDirtySuppression SuppressReplay;
				const bool bPresent = (Operation != ETransactionOperation::Undo) != bRemove;
				auto Reject = [&](EActorChangeError Code) -> FActorChangeResult {
					return {{.Code = Code, .TargetPath = Component ? Component->GetObjectPath() : std::string{}}};
				};
				auto Interrupted = []() -> FActorChangeResult {
					return {{.Code = EActorChangeError::TargetUnavailable,
						.CleanupCause = std::make_shared<FActorChangeError>(FActorChangeError{.Code = EActorChangeError::RollbackIncomplete})}};
				};
				if (!CanEditActorOwner(Owner.Get()) || !IsValid(Component.Get()) || Component->IsBeingDestroyed() || Component->HasBegunPlay()
					|| Component->GetOwner() != Owner.Get()
					|| Component->HasAnyObjectFlags(EObjectFlags::ClassDefaultObject | EObjectFlags::DefaultSubobject)
					|| Component->GetCreationMethod() == EComponentCreationMethod::Generated) return Reject(EActorChangeError::TargetUnavailable);
				if (Owner->IsInstanceComponent(Component.Get()) == bPresent) return Reject(EActorChangeError::MembershipChanged);
				auto* Scene = Cast<DSceneComponent>(Component.Get());
				if (bPresent)
				{
					if (Owner->OwnsComponent(Component.Get())) return Reject(EActorChangeError::MembershipChanged);
					if (bRoot && Owner->GetRootComponent()) return Reject(EActorChangeError::MembershipChanged);
					if (Parent && (!IsValid(Parent.Get()) || !Parent->GetOwner()
						|| Parent->GetOwner()->GetOuter() != Owner->GetOuter()
						|| !Parent->GetOwner()->OwnsComponent(Parent.Get()) || Parent.Get() == Scene
						|| (Scene && Parent->IsAttachedTo(Scene)))) return Reject(EActorChangeError::ParentInvalid);
					for (const auto& Existing : Owner->GetComponents())
						if (Existing && Existing->GetFName() == Component->GetFName()) return Reject(EActorChangeError::MembershipChanged);
					for (const auto& Entry : Children)
						if (!IsValid(Entry.Child.Get()) || Entry.Child->IsBeingDestroyed() || !Entry.Child->GetOwner()
							|| !Entry.Child->GetOwner()->OwnsComponent(Entry.Child.Get())
							|| Entry.Child->GetOwner()->GetOuter() != Owner->GetOuter()
							|| Entry.Child->GetAttachParent()) return Reject(EActorChangeError::ParentInvalid);
					// Runtime callbacks can retire participants after membership publication.
					Component->SetTransient(bTransient);
					if (Scene)
					{
						Scene->SetRelativeTransform(Relative);
						if (Parent) verify(Scene->AttachToComponent(Parent.Get(), EAttachmentTransformRule::KeepRelative));
						else Scene->UpdateComponentToWorld();
					}
					if (!Owner->SetInstanceComponentPresence(Component.Get(), true, false, InstanceIndex))
						return Reject(EActorChangeError::MembershipChanged);
					if (!CanEditActorOwner(Owner.Get()) || !IsValid(Component.Get()) || !Owner->IsInstanceComponent(Component.Get()))
						return Interrupted();
					if (bRoot) verify(Owner->SetRootComponent(Scene));
					for (const auto& Entry : Children)
					{
						verify(Entry.Child->AttachToComponent(Scene, EAttachmentTransformRule::KeepRelative));
						Entry.Child->SetRelativeTransform(Entry.Relative);
					}
					if (bRegistered) Component->RegisterComponent();
					if (!CanEditActorOwner(Owner.Get()) || !IsValid(Component.Get()) || !Owner->IsInstanceComponent(Component.Get()))
						return Interrupted();
					Retained.clear();
					RetainTree(Component.Get());
				}
				else
				{
					if (Scene && (Scene->GetAttachParent() != Parent.Get()
						|| (Owner->GetRootComponent() == Scene) != bRoot)) return Reject(EActorChangeError::ParentMismatch);
					if (Scene && Scene->GetAttachChildren().size() != Children.size()) return Reject(EActorChangeError::MembershipChanged);
					for (const auto& Entry : Children)
						if (!IsValid(Entry.Child.Get()) || Entry.Child->GetAttachParent() != Scene) return Reject(EActorChangeError::ParentMismatch);
					bRegistered = Component->IsRegistered();
					Retained.clear();
					RetainTree(Component.Get());
					if (!Owner->SetInstanceComponentPresence(Component.Get(), false)) return Reject(EActorChangeError::MembershipChanged);
					if (!CanEditActorOwner(Owner.Get()) || !IsValid(Component.Get()) || Owner->OwnsComponent(Component.Get()))
						return Interrupted();
					Retained.clear();
					RetainTree(Component.Get());
					Component->SetTransient(true);
				}
				return {};
			}

		private:
			auto RetainTree(DObject* Object) -> void
			{
				if (!IsValid(Object)) return;
				Retained.emplace_back(Object);
				for (DObject* Child : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::LiveOnly)) RetainTree(Child);
			}
			TObjectPtr<AActor> Owner;
			TObjectPtr<DActorComponent> Component;
			TObjectPtr<DSceneComponent> Parent;
			std::vector<FChildAttachment> Children;
			std::vector<TObjectPtr<DObject>> Retained;
			FTransform Relative;
			std::array<DPackage*, 1> Packages{};
			size_t InstanceIndex = 0;
			bool bRemove = false, bRoot = false, bRegistered = true, bTransient = false, bDuplicate = false;
		};

		class FComponentEditChange : public FActorCommandChange
		{
		public:
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{ return {Component.Get()}; }

			FComponentEditChange(AActor& InOwner, DActorComponent& InComponent)
				: Owner(&InOwner), Component(&InComponent), Packages{InOwner.GetPackage()} {}
			auto GetOwningModule() const -> std::string_view override { return "DurinEd"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (DObject* Object : {static_cast<DObject*>(Owner.Get()), static_cast<DObject*>(Component.Get())})
					if (Object) Collector.AddReferencedObject(Object);
			}
		protected:
			auto CanReplay() const -> bool
			{
				return CanEditActorOwner(Owner.Get()) && IsValid(Component.Get())
					&& !Component->IsBeingDestroyed() && !Component->HasBegunPlay()
					&& Component->GetOwner() == Owner.Get() && Owner->OwnsComponent(Component.Get())
					&& Component->GetCreationMethod() != EComponentCreationMethod::Generated;
			}
			auto Reject(EActorChangeError Code) const -> FActorChangeResult
			{ return {{.Code = Code, .TargetPath = Component ? Component->GetObjectPath() : std::string{}}}; }
			TObjectPtr<AActor> Owner;
			TObjectPtr<DActorComponent> Component;
		private:
			std::array<DPackage*, 1> Packages;
		};

		class FComponentRenameChange final : public FComponentEditChange
		{
		public:
			FComponentRenameChange(AActor& Owner, DActorComponent& Component, FName InAfter)
				: FComponentEditChange(Owner, Component), Before(Component.GetFName()), After(InAfter) {}
			auto GetDescription() const -> std::string_view override { return "Rename component"; }
			auto Replay(ETransactionOperation Operation) -> FActorChangeResult override
			{
				FScopedPackageDirtySuppression SuppressReplay;
				if (!CanReplay() || Component->IsTemplateObject()) return Reject(EActorChangeError::TargetUnavailable);
				const bool bAfter = Operation != ETransactionOperation::Undo;
				const FName Desired = bAfter ? After : Before;
				if (Component->GetFName() != (bAfter ? Before : After)) return Reject(EActorChangeError::MembershipChanged);
				for (DObject* Object : GDObjectArray.GetObjectsWithOuter(Owner.Get(), EObjectQueryScope::LiveOnly))
					if (Object != Component.Get() && Object->GetFName() == Desired) return Reject(EActorChangeError::MembershipChanged);
				Component->Rename(Desired);
				return {};
			}
		private:
			FName Before, After;
		};

		class FComponentReparentChange final : public FComponentEditChange
		{
		public:
			auto GetModifiedObjects() const -> std::vector<DObject*> override
			{ return {Component.Get(), BeforeParent.Get(), AfterParent.Get()}; }

			FComponentReparentChange(AActor& Owner, DSceneComponent& Component, DSceneComponent* Parent)
				: FComponentEditChange(Owner, Component), BeforeParent(Component.GetAttachParent()), AfterParent(Parent),
				BeforeRelative(Component.GetRelativeTransform()), AfterRelative(Parent
					? FTransform::MakeRelative(Component.GetWorldTransform(), Parent->GetWorldTransform()) : Component.GetWorldTransform()) {}
			auto GetDescription() const -> std::string_view override { return "Reparent component"; }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				FComponentEditChange::AddReferencedObjects(Collector);
				for (DObject* Object : {static_cast<DObject*>(BeforeParent.Get()), static_cast<DObject*>(AfterParent.Get())})
					if (Object) Collector.AddReferencedObject(Object);
			}
			auto Replay(ETransactionOperation Operation) -> FActorChangeResult override
			{
				FScopedPackageDirtySuppression SuppressReplay;
				if (!CanReplay() || Owner->GetRootComponent() == Component.Get()) return Reject(EActorChangeError::TargetUnavailable);
				auto* Scene = static_cast<DSceneComponent*>(Component.Get());
				const bool bAfter = Operation != ETransactionOperation::Undo;
				auto* Parent = (bAfter ? AfterParent : BeforeParent).Get();
				if (Scene->GetAttachParent() != (bAfter ? BeforeParent : AfterParent).Get()) return Reject(EActorChangeError::ParentMismatch);
				if (Parent)
				{
					if (!IsValid(Parent) || Parent->IsBeingDestroyed() || Parent->HasBegunPlay() || !Parent->GetOwner()
						|| !Parent->GetOwner()->OwnsComponent(Parent) || Parent->GetOwner()->GetOuter() != Owner->GetOuter()
						|| Parent->GetCreationMethod() == EComponentCreationMethod::Generated) return Reject(EActorChangeError::ParentInvalid);
					if (Parent == Scene || Parent->IsAttachedTo(Scene)) return Reject(EActorChangeError::ParentCycle);
					if (!Scene->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative)) return Reject(EActorChangeError::AttachmentWrite);
				}
				else if (Scene->GetAttachParent() && !Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative))
					return Reject(EActorChangeError::AttachmentWrite);
				Scene->SetRelativeTransform(bAfter ? AfterRelative : BeforeRelative);
				return {};
			}
		private:
			TObjectPtr<DSceneComponent> BeforeParent, AfterParent;
			FTransform BeforeRelative, AfterRelative;
		};

		auto Execute(std::unique_ptr<FActorCommandChange> Change, DActorComponent* Component,
			DTransactor* Transactions) -> bool
		{
			if (!Transactions) return false;
			const auto Result = ::Durin::Editor::ApplyEditorChange(*Transactions, std::move(Change));
			if (!Result && Result.Code != ETransactorResultCode::RecoveryRequired && IsValid(Component))
				if (auto* Owner = Component->GetOwner(); !IsValid(Owner) || !Owner->OwnsComponent(Component)) MarkObjectHierarchyAsGarbage(Component);
			if (!Result) return false;
			return true;
		}
		auto UniqueName(AActor& Owner, FName Requested, DObject* Ignored = nullptr) -> FName
		{
			const auto Existing = GDObjectArray.GetObjectsWithOuter(&Owner, EObjectQueryScope::LiveOnly);
			FName Name = Requested;
			for (uint32 Suffix = 2; std::ranges::any_of(Existing, [&](DObject* Object) { return Object != Ignored && Object->GetFName() == Name; }); ++Suffix)
				Name = FName(std::format("{}_{}", Requested, Suffix));
			return Name;
		}
	}

	auto DEditorActorSubsystem::AddComponent(AActor& Owner, DClass* Class, DSceneComponent* Parent,
		const FActorEditContext& Context) -> DActorComponent*
	{
		auto Admission = Admit(Context, nullptr, &Owner);
		if (!Admission) return nullptr;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> DActorComponent*
		{
			auto* Transactions = Admission;
			if (!CanConstructObjectOfClass(Class, DActorComponent::StaticClass())
				|| (Parent && (!IsValid(Parent) || Parent->IsBeingDestroyed() || Parent->HasBegunPlay()
					|| Parent->GetCreationMethod() == EComponentCreationMethod::Generated || Parent->GetOwner() != &Owner || !Owner.OwnsComponent(Parent)))) return nullptr;
			DActorComponent* Component = nullptr;
			{
				FScopedPackageDirtySuppression SuppressPreparation;
				Component = NewObject<DActorComponent>(Class, &Owner, UniqueName(Owner, FName(Class->GetDefaultObjectName())), EObjectFlags::Transactional);
			}
			if (!Component) return nullptr;
			Component->SetTransient(true);
			auto Change = std::make_unique<FComponentMembershipChange>(Owner, *Component, false);
			auto* Scene = Cast<DSceneComponent>(Component);
			DSceneComponent* Destination = Scene ? (Parent ? Parent : Owner.GetRootComponent()) : nullptr;
			FTransform Relative = Scene && !Parent ? Scene->GetRelativeTransform() : FTransform{};
			if (Scene && !Parent && Destination)
			{
				Scene->UpdateComponentToWorld();
				Relative = FTransform::MakeRelative(Scene->GetWorldTransform(), Destination->GetWorldTransform());
			}
			Change->ConfigureAddition(Destination, Scene && !Owner.GetRootComponent() && !Destination, Relative);
			if (auto Revalidated = Admit(Context, nullptr, &Owner, true); !Revalidated)
			{
				if (IsValid(Component)) MarkObjectHierarchyAsGarbage(Component);
				return nullptr;
			}
			if (auto Applied = Execute(std::move(Change), Component, Transactions); !Applied) return nullptr;
			return Component;

		});
	}

	auto DEditorActorSubsystem::DuplicateComponent(AActor& Owner, DActorComponent* Source,
		const FActorEditContext& Context) -> DActorComponent*
	{
		auto Admission = Admit(Context, nullptr, &Owner);
		if (!Admission) return nullptr;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> DActorComponent*
		{
			auto* Transactions = Admission;
			if (!IsValid(Source) || !Owner.IsInstanceComponent(Source)
				|| Owner.GetRootComponent() == Source) return nullptr;
			auto Copy = [&] {
				FScopedPackageDirtySuppression SuppressPreparation;
				auto Prepared = DuplicateObject(Source, &Owner, UniqueName(Owner, Source->GetFName()));
				if (Prepared)
					if (auto* Scene = Cast<DSceneComponent>(*Prepared); Scene && Scene->GetAttachParent())
						Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
				return Prepared;
			}();
			if (!Copy) return nullptr;
			auto* Component = *Copy;
			auto Change = std::make_unique<FComponentMembershipChange>(Owner, *Component, false);
			Component->SetTransient(true);
			// Ordinary duplication runs PostLoad and remaps owned subobjects before publication.
			auto* SourceScene = Cast<DSceneComponent>(Source);
			Change->ConfigureAddition(SourceScene ? SourceScene->GetAttachParent() : nullptr, false,
				SourceScene ? SourceScene->GetRelativeTransform() : FTransform{}, Source->HasAnyObjectFlags(EObjectFlags::Transient), true);
			if (auto Revalidated = Admit(Context, nullptr, &Owner, true); !Revalidated)
			{
				if (IsValid(Component)) MarkObjectHierarchyAsGarbage(Component);
				return nullptr;
			}
			if (auto Applied = Execute(std::move(Change), Component, Transactions); !Applied) return nullptr;
			return Component;

		});
	}

	auto DEditorActorSubsystem::RenameComponent(AActor& Owner, DActorComponent* Component, FName RequestedName,
		const FActorEditContext& Context) -> bool
	{
		auto Admission = Admit(Context, nullptr, &Owner);
		if (!Admission) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			auto* Transactions = Admission;
			if (!IsValid(Component) || Component->IsTemplateObject()
				|| Component->GetOwner() != &Owner || !Owner.OwnsComponent(Component) || RequestedName.IsNone()
				|| Component->GetCreationMethod() == EComponentCreationMethod::Generated) return false;
			const FName Name = UniqueName(Owner, RequestedName, Component);
			if (Name == Component->GetFName()) return true;
			return Execute(std::make_unique<FComponentRenameChange>(Owner, *Component, Name), Component, Transactions);

		});
	}

	auto DEditorActorSubsystem::ReparentComponent(AActor& Owner, DSceneComponent* Component, DSceneComponent* Parent,
		const FActorEditContext& Context) -> bool
	{
		auto Admission = Admit(Context, nullptr, &Owner);
		if (!Admission) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			auto* Transactions = Admission;
			if (!IsValid(Component) || Component->GetOwner() != &Owner
				|| !Owner.OwnsComponent(Component) || Owner.GetRootComponent() == Component
				|| Component->GetCreationMethod() == EComponentCreationMethod::Generated
				|| (Parent && (!IsValid(Parent) || Parent->GetOwner() != &Owner || !Owner.OwnsComponent(Parent)
					|| Parent->GetCreationMethod() == EComponentCreationMethod::Generated || Parent == Component || Parent->IsAttachedTo(Component))))
				return false;
			if (Component->GetAttachParent() == Parent) return true;
			return Execute(std::make_unique<FComponentReparentChange>(Owner, *Component, Parent), Component, Transactions);

		});
	}

	auto DEditorActorSubsystem::DeleteComponent(AActor& Owner, DActorComponent* Component,
		const FActorEditContext& Context) -> bool
	{
		auto Admission = Admit(Context, nullptr, &Owner);
		if (!Admission) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			auto* Transactions = Admission;
			if (!IsValid(Component) || !Owner.IsInstanceComponent(Component)
				|| Component->GetCreationMethod() == EComponentCreationMethod::Generated)
				return false;
			return Execute(std::make_unique<FComponentMembershipChange>(Owner, *Component, true), Component, Transactions);

		});
	}
}
