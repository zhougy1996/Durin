#include "Operations/ComponentMutations.h"

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
#include "Transactions/Transactor.h"

namespace Durin::Editor::Level
{
	namespace
	{
		struct FChildAttachment
		{
			TObjectPtr<DSceneComponent> Child;
			FTransform Relative;
		};

		// Keeps an absent component alive, rather than resurrecting a garbage object or
		// replacing its identity and invalidating older property-history records.
		class FComponentMembershipChange final : public ITransactionCustomChange
		{
		public:
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
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
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
			auto Replay(ETransactionOperation Operation) -> FTransactionCustomResult override
			{
				const bool bPresent = (Operation != ETransactionOperation::Undo) != bRemove;
				auto Reject = [&](ETransactionCustomError Code) -> FTransactionCustomResult {
					return {{.Code = Code, .TargetPath = Component ? Component->GetObjectPath() : std::string{}}};
				};
				if (!IsValid(Owner.Get()) || !IsValid(Component.Get()) || Owner->IsBeingDestroyed()
					|| Owner->HasBegunPlay() || Component->IsBeingDestroyed() || Component->HasBegunPlay()
					|| Component->GetOwner() != Owner.Get()
					|| Component->HasAnyObjectFlags(EObjectFlags::ClassDefaultObject | EObjectFlags::DefaultSubobject)
					|| Component->GetCreationMethod() == EComponentCreationMethod::Generated) return Reject(ETransactionCustomError::TargetUnavailable);
				if (Owner->IsInstanceComponent(Component.Get()) == bPresent) return Reject(ETransactionCustomError::MembershipChanged);
				auto* Scene = Cast<DSceneComponent>(Component.Get());
				if (bPresent)
				{
					if (Owner->OwnsComponent(Component.Get())) return Reject(ETransactionCustomError::MembershipChanged);
					if (bRoot && Owner->GetRootComponent()) return Reject(ETransactionCustomError::MembershipChanged);
					if (Parent && (!IsValid(Parent.Get()) || !Parent->GetOwner()
						|| Parent->GetOwner()->GetOuter() != Owner->GetOuter()
						|| !Parent->GetOwner()->OwnsComponent(Parent.Get()) || Parent.Get() == Scene
						|| (Scene && Parent->IsAttachedTo(Scene)))) return Reject(ETransactionCustomError::ParentInvalid);
					for (const auto& Existing : Owner->GetComponents())
						if (Existing && Existing->GetFName() == Component->GetFName()) return Reject(ETransactionCustomError::MembershipChanged);
					for (const auto& Entry : Children)
						if (!IsValid(Entry.Child.Get()) || Entry.Child->IsBeingDestroyed() || !Entry.Child->GetOwner()
							|| !Entry.Child->GetOwner()->OwnsComponent(Entry.Child.Get())
							|| Entry.Child->GetOwner()->GetOuter() != Owner->GetOuter()
							|| Entry.Child->GetAttachParent()) return Reject(ETransactionCustomError::ParentInvalid);
					// No potentially rejecting writes remain after membership is published.
					Component->SetTransient(bTransient);
					if (Scene)
					{
						Scene->SetRelativeTransform(Relative);
						if (Parent) verify(Scene->AttachToComponent(Parent.Get(), EAttachmentTransformRule::KeepRelative));
						else Scene->UpdateComponentToWorld();
					}
					if (!Owner->SetInstanceComponentPresence(Component.Get(), true, false, InstanceIndex))
						return Reject(ETransactionCustomError::MembershipChanged);
					if (bRoot) verify(Owner->SetRootComponent(Scene));
					for (const auto& Entry : Children)
					{
						verify(Entry.Child->AttachToComponent(Scene, EAttachmentTransformRule::KeepRelative));
						Entry.Child->SetRelativeTransform(Entry.Relative);
					}
					if (bRegistered) Component->RegisterComponent();
				}
				else
				{
					if (Scene && (Scene->GetAttachParent() != Parent.Get()
						|| (Owner->GetRootComponent() == Scene) != bRoot)) return Reject(ETransactionCustomError::ParentMismatch);
					if (Scene && Scene->GetAttachChildren().size() != Children.size()) return Reject(ETransactionCustomError::MembershipChanged);
					for (const auto& Entry : Children)
						if (!IsValid(Entry.Child.Get()) || Entry.Child->GetAttachParent() != Scene) return Reject(ETransactionCustomError::ParentMismatch);
					if (!Owner->SetInstanceComponentPresence(Component.Get(), false)) return Reject(ETransactionCustomError::MembershipChanged);
					Component->SetTransient(true);
				}
				return {};
			}

		private:
			auto RetainTree(DObject* Object) -> void
			{
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

		class FComponentEditChange : public ITransactionCustomChange
		{
		public:
			FComponentEditChange(AActor& InOwner, DActorComponent& InComponent)
				: Owner(&InOwner), Component(&InComponent), Packages{InOwner.GetPackage()} {}
			auto GetOwningModule() const -> std::string_view override { return "LevelEditor"; }
			auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Packages; }
			auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override
			{
				for (DObject* Object : {static_cast<DObject*>(Owner.Get()), static_cast<DObject*>(Component.Get())})
					if (Object) Collector.AddReferencedObject(Object);
			}
		protected:
			auto CanReplay() const -> bool
			{
				return IsValid(Owner.Get()) && IsValid(Component.Get()) && !Owner->IsTemplateObject()
					&& !Owner->IsBeingDestroyed() && !Owner->HasBegunPlay()
					&& !Component->IsBeingDestroyed() && !Component->HasBegunPlay()
					&& Component->GetOwner() == Owner.Get() && Owner->OwnsComponent(Component.Get())
					&& Component->GetCreationMethod() != EComponentCreationMethod::Generated;
			}
			auto Reject(ETransactionCustomError Code) const -> FTransactionCustomResult
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
			auto Replay(ETransactionOperation Operation) -> FTransactionCustomResult override
			{
				if (!CanReplay() || Component->IsTemplateObject()) return Reject(ETransactionCustomError::TargetUnavailable);
				const bool bAfter = Operation != ETransactionOperation::Undo;
				const FName Desired = bAfter ? After : Before;
				if (Component->GetFName() != (bAfter ? Before : After)) return Reject(ETransactionCustomError::MembershipChanged);
				for (DObject* Object : GDObjectArray.GetObjectsWithOuter(Owner.Get(), EObjectQueryScope::LiveOnly))
					if (Object != Component.Get() && Object->GetFName() == Desired) return Reject(ETransactionCustomError::MembershipChanged);
				Component->Rename(Desired);
				return {};
			}
		private:
			FName Before, After;
		};

		class FComponentReparentChange final : public FComponentEditChange
		{
		public:
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
			auto Replay(ETransactionOperation Operation) -> FTransactionCustomResult override
			{
				if (!CanReplay() || Owner->GetRootComponent() == Component.Get()) return Reject(ETransactionCustomError::TargetUnavailable);
				auto* Scene = static_cast<DSceneComponent*>(Component.Get());
				const bool bAfter = Operation != ETransactionOperation::Undo;
				auto* Parent = (bAfter ? AfterParent : BeforeParent).Get();
				if (Scene->GetAttachParent() != (bAfter ? BeforeParent : AfterParent).Get()) return Reject(ETransactionCustomError::ParentMismatch);
				if (Parent)
				{
					if (!IsValid(Parent) || Parent->IsBeingDestroyed() || Parent->HasBegunPlay() || !Parent->GetOwner()
						|| !Parent->GetOwner()->OwnsComponent(Parent) || Parent->GetOwner()->GetOuter() != Owner->GetOuter()
						|| Parent->GetCreationMethod() == EComponentCreationMethod::Generated) return Reject(ETransactionCustomError::ParentInvalid);
					if (Parent == Scene || Parent->IsAttachedTo(Scene)) return Reject(ETransactionCustomError::ParentCycle);
					if (!Scene->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative)) return Reject(ETransactionCustomError::AttachmentWrite);
				}
				else if (Scene->GetAttachParent() && !Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative))
					return Reject(ETransactionCustomError::AttachmentWrite);
				Scene->SetRelativeTransform(bAfter ? AfterRelative : BeforeRelative);
				return {};
			}
		private:
			TObjectPtr<DSceneComponent> BeforeParent, AfterParent;
			FTransform BeforeRelative, AfterRelative;
		};

		auto Execute(std::unique_ptr<ITransactionCustomChange> Change, DActorComponent* Component,
			DTransactor* Transactions) -> FComponentMutationResult
		{
			if (!Transactions) return {nullptr, "Component editing requires an editor transactor."};
			const auto Result = Transactions->Execute(std::move(Change));
			return Result ? FComponentMutationResult{Component, {}} : FComponentMutationResult{nullptr, FormatTransactorResult(Result)};
		}
		auto CanEdit(AActor& Owner, bool bReadOnly) -> bool
		{
			return !bReadOnly && IsValid(&Owner) && !Owner.IsTemplateObject() && !Owner.IsBeingDestroyed() && !Owner.HasBegunPlay();
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

	auto FComponentMutations::Add(AActor& Owner, DClass* Class, DSceneComponent* Parent,
		DTransactor* Transactions, bool bReadOnly) -> FComponentMutationResult
	{
		if (!CanEdit(Owner, bReadOnly) || !Transactions || !CanConstructObjectOfClass(Class, DActorComponent::StaticClass())
			|| (Parent && (Parent->GetOwner() != &Owner || !Owner.OwnsComponent(Parent)))) return {nullptr, "Invalid component addition target."};
		DActorComponent* Component = nullptr;
		{
			FScopedPackageDirtySuppression SuppressPreparation;
			Component = NewObject<DActorComponent>(Class, &Owner, UniqueName(Owner, FName(Class->GetDefaultObjectName())));
		}
		if (!Component) return {nullptr, "Component construction failed."};
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
		return Execute(std::move(Change), Component, Transactions);
	}

	auto FComponentMutations::Duplicate(AActor& Owner, DActorComponent* Source,
		DTransactor* Transactions, bool bReadOnly) -> FComponentMutationResult
	{
		if (!CanEdit(Owner, bReadOnly) || !Transactions || !IsValid(Source) || !Owner.IsInstanceComponent(Source)
			|| Owner.GetRootComponent() == Source) return {nullptr, "Invalid component duplication target."};
		auto Copy = [&] {
			FScopedPackageDirtySuppression SuppressPreparation;
			auto Prepared = DuplicateObject(Source, &Owner, UniqueName(Owner, Source->GetFName()));
			if (Prepared)
				if (auto* Scene = Cast<DSceneComponent>(*Prepared); Scene && Scene->GetAttachParent())
					Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
			return Prepared;
		}();
		if (!Copy) return {nullptr, ToString(Copy.error())};
		auto* Component = *Copy;
		auto Change = std::make_unique<FComponentMembershipChange>(Owner, *Component, false);
		Component->SetTransient(true);
		// Ordinary duplication runs PostLoad and remaps owned subobjects before publication.
		auto* SourceScene = Cast<DSceneComponent>(Source);
		Change->ConfigureAddition(SourceScene ? SourceScene->GetAttachParent() : nullptr, false,
			SourceScene ? SourceScene->GetRelativeTransform() : FTransform{}, Source->HasAnyObjectFlags(EObjectFlags::Transient), true);
		return Execute(std::move(Change), Component, Transactions);
	}

	auto FComponentMutations::Rename(AActor& Owner, DActorComponent* Component, FName RequestedName,
		DTransactor* Transactions, bool bReadOnly) -> FComponentMutationResult
	{
		if (!CanEdit(Owner, bReadOnly) || !Transactions || !IsValid(Component) || Component->IsTemplateObject()
			|| Component->GetOwner() != &Owner || !Owner.OwnsComponent(Component) || RequestedName.IsNone()
			|| Component->GetCreationMethod() == EComponentCreationMethod::Generated) return {nullptr, "Invalid component rename target."};
		const FName Name = UniqueName(Owner, RequestedName, Component);
		if (Name == Component->GetFName()) return {Component, {}};
		return Execute(std::make_unique<FComponentRenameChange>(Owner, *Component, Name), Component, Transactions);
	}

	auto FComponentMutations::Reparent(AActor& Owner, DSceneComponent* Component, DSceneComponent* Parent,
		DTransactor* Transactions, bool bReadOnly) -> FComponentMutationResult
	{
		if (!CanEdit(Owner, bReadOnly) || !Transactions || !IsValid(Component) || Component->GetOwner() != &Owner
			|| !Owner.OwnsComponent(Component) || Owner.GetRootComponent() == Component
			|| Component->GetCreationMethod() == EComponentCreationMethod::Generated
			|| (Parent && (!IsValid(Parent) || Parent->GetOwner() != &Owner || !Owner.OwnsComponent(Parent)
				|| Parent->GetCreationMethod() == EComponentCreationMethod::Generated || Parent == Component || Parent->IsAttachedTo(Component))))
			return {nullptr, "Invalid component attachment target."};
		if (Component->GetAttachParent() == Parent) return {Component, {}};
		return Execute(std::make_unique<FComponentReparentChange>(Owner, *Component, Parent), Component, Transactions);
	}

	auto FComponentMutations::Remove(AActor& Owner, DActorComponent* Component,
		DTransactor* Transactions, bool bReadOnly) -> FComponentMutationResult
	{
		if (!CanEdit(Owner, bReadOnly) || !Transactions || !IsValid(Component) || !Owner.IsInstanceComponent(Component))
			return {nullptr, "Invalid component deletion target."};
		return Execute(std::make_unique<FComponentMembershipChange>(Owner, *Component, true), Component, Transactions);
	}
}
