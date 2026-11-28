#include "Actors/EditorActorSubsystem.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorEditingScope.h"
#include "Actors/ActorEditingValidation.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "DObject/Archive.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"

namespace Durin
{
	namespace
	{
		auto PublishInstance(AActor& Owner, DActorComponent& Component, DSceneComponent* Parent,
			const FTransform& Relative, bool bTransient, DTransactor& Transactions, std::string_view Description) -> bool
		{
			TStrongObjectPtr<DActorComponent> Prepared(&Component);
			Component.SetCreationMethod(EComponentCreationMethod::Instance);
			FScopedTransaction Scope("Component", Description, &Owner);
			if (!Scope.IsOutstanding()) return false;
			Owner.SetFlags(EObjectFlags::Transactional);
			Owner.Modify(false);
			if (!Transactions.IsActive()) return false;
			Component.SetFlags(EObjectFlags::Transactional);
			Component.Modify(false);
			if (!Transactions.IsActive()) return false;
			Component.SetTransient(bTransient);
			auto* Scene = Cast<DSceneComponent>(&Component);
			if (Scene)
			{
				Scene->SetRelativeTransform(Relative);
				if (Parent && !Scene->AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative)) return false;
				Scene->UpdateComponentToWorld();
			}
			if (!Owner.SetInstanceComponentPresence(&Component, true, false) || !IsValid(&Component)
				|| !CanEditActorOwner(&Owner) || !Owner.IsInstanceComponent(&Component)) return false;
			if (Scene && !Parent && !Owner.GetRootComponent()) Owner.SetRootComponent(Scene);
			Component.RegisterComponent();
			return CanEditActorOwner(&Owner) && IsValid(&Component) && Owner.IsInstanceComponent(&Component);
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
			DObjectForceRegistration(Class);
			const auto Name = Class->GetDefaultObjectName().empty() ? Class->GetName() : Class->GetDefaultObjectName();
			auto* Component = NewObject<DActorComponent>(Class, &Owner, UniqueName(Owner, FName(Name)),
				EObjectFlags::Transactional | EObjectFlags::Transient);
			if (!Component) return nullptr;
			if (IsValid(&Owner) && IsValid(Component)) Owner.RemoveOwnedComponent(Component);
			auto* Scene = Cast<DSceneComponent>(Component);
			DSceneComponent* Destination = Scene ? (Parent ? Parent : Owner.GetRootComponent()) : nullptr;
			FTransform Relative = Scene && !Parent ? Scene->GetRelativeTransform() : FTransform{};
			if (Scene && !Parent && Destination)
			{
				Scene->UpdateComponentToWorld();
				Relative = FTransform::MakeRelative(Scene->GetWorldTransform(), Destination->GetWorldTransform());
			}
			if (auto Revalidated = Admit(Context, nullptr, &Owner, true); !Revalidated)
			{
				if (IsValid(Component)) MarkObjectHierarchyAsGarbage(Component);
				return nullptr;
			}
			if (!PublishInstance(Owner, *Component, Destination, Relative, false, *Transactions, "Add component"))
			{
				if (IsValid(Component) && !Owner.OwnsComponent(Component)) MarkObjectHierarchyAsGarbage(Component);
				return nullptr;
			}
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
			auto Copy = StaticDuplicateObjectEx({.SourceObject = Source, .DestOuter = &Owner,
				.DestName = UniqueName(Owner, Source->GetFName()), .ApplyFlags = EObjectFlags::Transient});
			if (!Copy) return nullptr;
			auto* Component = Cast<DActorComponent>(*Copy);
			if (auto* Scene = Cast<DSceneComponent>(Component); Scene && Scene->GetAttachParent())
				Scene->DetachFromComponent(EDetachmentTransformRule::KeepRelative);
			if (IsValid(&Owner) && IsValid(Component)) Owner.RemoveOwnedComponent(Component);
			// Ordinary duplication runs PostLoad and remaps owned subobjects before publication.
			auto* SourceScene = Cast<DSceneComponent>(Source);
			if (auto Revalidated = Admit(Context, nullptr, &Owner, true); !Revalidated)
			{
				if (IsValid(Component)) MarkObjectHierarchyAsGarbage(Component);
				return nullptr;
			}
			if (!PublishInstance(Owner, *Component, SourceScene ? SourceScene->GetAttachParent() : nullptr,
				SourceScene ? SourceScene->GetRelativeTransform() : FTransform{}, Source->HasAnyObjectFlags(EObjectFlags::Transient),
				*Transactions, "Duplicate component"))
			{
				if (IsValid(Component) && !Owner.OwnsComponent(Component)) MarkObjectHierarchyAsGarbage(Component);
				return nullptr;
			}
			return Component;

		});
	}

	auto DEditorActorSubsystem::RenameComponent(AActor& Owner, DActorComponent* Component, FName RequestedName,
		const FActorEditContext& Context) -> bool
	{
		if (!Admit(Context, nullptr, &Owner)) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			if (!IsValid(Component) || Component->IsTemplateObject()
				|| Component->GetOwner() != &Owner || !Owner.OwnsComponent(Component) || RequestedName.IsNone()
				|| Component->GetCreationMethod() == EComponentCreationMethod::Generated) return false;
			const FName Name = UniqueName(Owner, RequestedName, Component);
			if (Name == Component->GetFName()) return true;
			FScopedTransaction Recording("ComponentRename", "Rename component", Component);
			if (!Recording.IsOutstanding()) return false;
			Component->SetFlags(EObjectFlags::Transactional);
			(void)Component->Modify();
			return Component->Rename(Name, ERenameFlags::DoNotDirty);

		});
	}

	auto DEditorActorSubsystem::ReparentComponent(AActor& Owner, DSceneComponent* Component, DSceneComponent* Parent,
		const FActorEditContext& Context) -> bool
	{
		if (!Admit(Context, nullptr, &Owner)) return false;
		return RunActorEdit(GetEditor(), bEditing, [&]() -> bool
		{
			if (!IsValid(Component) || Component->IsBeingDestroyed() || Component->HasBegunPlay() || Component->GetOwner() != &Owner
				|| !Owner.OwnsComponent(Component) || Owner.GetRootComponent() == Component
				|| Component->GetCreationMethod() == EComponentCreationMethod::Generated
				|| (Parent && (!IsValid(Parent) || Parent->IsBeingDestroyed() || Parent->HasBegunPlay() || Parent->GetOwner() != &Owner || !Owner.OwnsComponent(Parent)
					|| Parent->GetCreationMethod() == EComponentCreationMethod::Generated || Parent == Component || Parent->IsAttachedTo(Component))))
				return false;
			if (Component->GetAttachParent() == Parent) return true;
			FScopedTransaction Scope("ComponentAttachment", "Reparent component", Component);
			if (!Scope.IsOutstanding()) return false;
			Component->SetFlags(EObjectFlags::Transactional);
			(void)Component->Modify();
			return Parent ? Component->AttachToComponent(Parent, EAttachmentTransformRule::KeepWorld)
				: Component->DetachFromComponent(EDetachmentTransformRule::KeepWorld);

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
			FScopedTransaction Scope("Component", "Delete component", &Owner);
			if (!Scope.IsOutstanding()) return false;
			Owner.SetFlags(EObjectFlags::Transactional);
			Owner.Modify(false);
			Component->SetFlags(EObjectFlags::Transactional);
			Component->Modify(false);
			if (auto* Scene = Cast<DSceneComponent>(Component))
				for (const auto& Child : Scene->GetAttachChildren())
					if (Child) { Child->SetFlags(EObjectFlags::Transactional); Child->Modify(false); }
			if (!Transactions->IsActive() || !Owner.SetInstanceComponentPresence(Component, false)) return false;
			Component->SetTransient(true);
			return CanEditActorOwner(&Owner) && !Owner.OwnsComponent(Component);

		});
	}
}
