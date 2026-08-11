#include "Components/SceneComponent.h"

#include "DObject/Property.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		auto IsAppliedChange(const FPropertyChangedEvent& Event) -> bool
		{
			return Event.Phase != EPropertyChangePhase::Committed || Event.Origin != EPropertyChangeOrigin::Edit;
		}

		auto AreTransformsEqual(const FTransform& Left, const FTransform& Right) -> bool
		{
			return Left.Translation == Right.Translation
				&& Left.Rotation == Right.Rotation
				&& Left.Scale3D == Right.Scale3D;
		}
	}

	auto DSceneComponent::OnRegister() -> void
	{
		Super::OnRegister();
		AActor* Owner = GetOwner();
		DLevel* Level = Owner ? Cast<DLevel>(Owner->GetOuter()) : nullptr;
		DWorld* World = Level ? Level->GetWorld() : nullptr;
		RenderScene = World ? World->GetRenderScene() : nullptr;
	}

	auto DSceneComponent::OnUnregister() -> void
	{
		Super::OnUnregister();
		RenderScene = nullptr;
	}

	auto DSceneComponent::PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool
	{
		if (!Super::PreEditChangeProperty(Proposal, OutError)) return false;
		if (Proposal.MemberProperty && Proposal.MemberProperty->NamePrivate == FName("RelativeTransform")
			&& Proposal.DraftRootProperty == Proposal.MemberProperty && Proposal.DraftRootContainer)
		{
			auto* Transform = Proposal.DraftRootProperty->ContainerPtrToValuePtr<FTransform>(
				Proposal.DraftRootContainer, Proposal.DraftRootArrayIndex);
			Transform->Rotation = Math::Normalize(Transform->Rotation);
		}
		return true;
	}

	auto DSceneComponent::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (IsAppliedChange(Event) && Event.MemberProperty
			&& Event.MemberProperty->NamePrivate == FName("RelativeTransform")) UpdateComponentToWorld();
	}

	auto DSceneComponent::BeginDestroy() -> void
	{
		Super::BeginDestroy();
	}

	auto DSceneComponent::OnComponentPendingKill() -> void
	{
		const std::vector<TObjectPtr<DSceneComponent>> Children = AttachChildren;
		for (const TObjectPtr<DSceneComponent>& ChildPtr : Children)
		{
			if (DSceneComponent* Child = ChildPtr.Get(); Child && Child->GetAttachParent() == this)
			{
				Child->DetachFromComponent(EDetachmentTransformRule::KeepWorld);
			}
		}
		AttachChildren.clear();
		DetachFromComponent(EDetachmentTransformRule::KeepWorld);
		Super::OnComponentPendingKill();
	}

	auto DSceneComponent::GetRelativeTransform() const -> const FTransform&
	{
		return RelativeTransform;
	}

	auto DSceneComponent::SetRelativeTransform(const FTransform& InTransform) -> void
	{
		FTransform NewRelativeTransform = InTransform;
		NewRelativeTransform.Rotation = Math::Normalize(NewRelativeTransform.Rotation);
		if (AreTransformsEqual(RelativeTransform, NewRelativeTransform)) return;
		RelativeTransform = NewRelativeTransform;
		UpdateComponentToWorld();
		MarkPackageDirty();
	}

	auto DSceneComponent::GetWorldTransform() const -> const FTransform&
	{
		return ComponentToWorld;
	}

	auto DSceneComponent::SetWorldTransform(const FTransform& InTransform) -> void
	{
		FTransform NewWorldTransform = InTransform;
		NewWorldTransform.Rotation = Math::Normalize(NewWorldTransform.Rotation);
		if (AreTransformsEqual(ComponentToWorld, NewWorldTransform)) return;

		FTransform NewRelativeTransform;
		if (DSceneComponent* Parent = AttachParent.Get())
		{
			NewRelativeTransform = FTransform::MakeRelative(InTransform, Parent->GetWorldTransform());
		}
		else
		{
			NewRelativeTransform = InTransform;
		}
		NewRelativeTransform.Rotation = Math::Normalize(NewRelativeTransform.Rotation);
		if (AreTransformsEqual(RelativeTransform, NewRelativeTransform)) return;
		RelativeTransform = NewRelativeTransform;
		UpdateComponentToWorld();
		MarkPackageDirty();
	}

	auto DSceneComponent::GetRelativeLocation() const -> const FVector3&
	{
		return RelativeTransform.Translation;
	}

	auto DSceneComponent::SetRelativeLocation(const FVector3& InLocation) -> void
	{
		FTransform Transform = RelativeTransform;
		Transform.Translation = InLocation;
		SetRelativeTransform(Transform);
	}

	auto DSceneComponent::GetRelativeRotation() const -> const FQuat&
	{
		return RelativeTransform.Rotation;
	}

	auto DSceneComponent::SetRelativeRotation(const FQuat& InRotation) -> void
	{
		FTransform Transform = RelativeTransform;
		Transform.Rotation = InRotation;
		SetRelativeTransform(Transform);
	}

	auto DSceneComponent::GetRelativeScale3D() const -> const FVector3&
	{
		return RelativeTransform.Scale3D;
	}

	auto DSceneComponent::SetRelativeScale3D(const FVector3& InScale) -> void
	{
		FTransform Transform = RelativeTransform;
		Transform.Scale3D = InScale;
		SetRelativeTransform(Transform);
	}

	auto DSceneComponent::GetWorldLocation() const -> const FVector3&
	{
		return ComponentToWorld.Translation;
	}

	auto DSceneComponent::SetWorldLocation(const FVector3& InLocation) -> void
	{
		FTransform Transform = ComponentToWorld;
		Transform.Translation = InLocation;
		SetWorldTransform(Transform);
	}

	auto DSceneComponent::GetWorldRotation() const -> const FQuat&
	{
		return ComponentToWorld.Rotation;
	}

	auto DSceneComponent::SetWorldRotation(const FQuat& InRotation) -> void
	{
		FTransform Transform = ComponentToWorld;
		Transform.Rotation = InRotation;
		SetWorldTransform(Transform);
	}

	auto DSceneComponent::GetWorldScale3D() const -> const FVector3&
	{
		return ComponentToWorld.Scale3D;
	}

	auto DSceneComponent::SetWorldScale3D(const FVector3& InScale) -> void
	{
		FTransform Transform = ComponentToWorld;
		Transform.Scale3D = InScale;
		SetWorldTransform(Transform);
	}

	auto DSceneComponent::SetupAttachment(DSceneComponent* Parent) -> bool
	{
		return AttachToComponent(Parent, EAttachmentTransformRule::KeepRelative);
	}

	auto DSceneComponent::AttachToComponent(DSceneComponent* Parent, EAttachmentTransformRule Rule) -> bool
	{
		if (!CanAttachTo(Parent))
		{
			return false;
		}

#if DURIN_WITH_EDITOR
		AActor* Owner = GetOwner();
		const bool bActorRoot = Owner && Owner->GetRootComponent() == this;
		AActor* PreviousParentActor = bActorRoot ? Owner->GetAttachParentActor() : nullptr;
#endif
		const FTransform PreviousWorld = ComponentToWorld;
		if (DSceneComponent* PreviousParent = AttachParent.Get())
		{
			PreviousParent->RemoveAttachChild(this);
		}

		AttachParent = Parent;
		if (std::ranges::none_of(Parent->AttachChildren, [this](const TObjectPtr<DSceneComponent>& Child) { return Child.Get() == this; }))
		{
			Parent->AttachChildren.emplace_back(this);
		}

		switch (Rule)
		{
		case EAttachmentTransformRule::KeepWorld:
			RelativeTransform = FTransform::MakeRelative(PreviousWorld, Parent->GetWorldTransform());
			break;
		case EAttachmentTransformRule::KeepRelative:
			break;
		case EAttachmentTransformRule::SnapToTarget:
			RelativeTransform = FTransform();
			break;
		}

		UpdateComponentToWorld();
		MarkPackageDirty();
#if DURIN_WITH_EDITOR
		if (bActorRoot && PreviousParentActor != Owner->GetAttachParentActor())
		{
			if (auto* Level = Cast<DLevel>(Owner->GetOuter())) Level->NotifyEditorActorHierarchyChanged();
		}
#endif
		return true;
	}

	auto DSceneComponent::DetachFromComponent(EDetachmentTransformRule Rule) -> bool
	{
		DSceneComponent* Parent = AttachParent.Get();
		if (!Parent)
		{
			return false;
		}

#if DURIN_WITH_EDITOR
		AActor* Owner = GetOwner();
		const bool bActorRoot = Owner && Owner->GetRootComponent() == this;
		AActor* PreviousParentActor = bActorRoot ? Owner->GetAttachParentActor() : nullptr;
#endif
		const FTransform PreviousWorld = ComponentToWorld;
		Parent->RemoveAttachChild(this);
		AttachParent = nullptr;
		if (Rule == EDetachmentTransformRule::KeepWorld)
		{
			RelativeTransform = PreviousWorld;
		}
		UpdateComponentToWorld();
		MarkPackageDirty();
#if DURIN_WITH_EDITOR
		if (bActorRoot && PreviousParentActor != Owner->GetAttachParentActor())
		{
			if (auto* Level = Cast<DLevel>(Owner->GetOuter())) Level->NotifyEditorActorHierarchyChanged();
		}
#endif
		return true;
	}

	auto DSceneComponent::IsAttachedTo(const DSceneComponent* Component) const -> bool
	{
		for (const DSceneComponent* Parent = AttachParent.Get(); Parent; Parent = Parent->GetAttachParent())
		{
			if (Parent == Component)
			{
				return true;
			}
		}
		return false;
	}

	auto DSceneComponent::UpdateComponentToWorld() -> void
	{
		if (DSceneComponent* Parent = AttachParent.Get())
		{
			ComponentToWorld = FTransform::Combine(Parent->GetWorldTransform(), RelativeTransform);
		}
		else
		{
			ComponentToWorld = RelativeTransform;
		}

		OnUpdateTransform();
		for (const TObjectPtr<DSceneComponent>& ChildPtr : AttachChildren)
		{
			if (DSceneComponent* Child = ChildPtr.Get(); Child && Child->GetAttachParent() == this)
			{
				Child->UpdateComponentToWorld();
			}
		}
	}

	auto DSceneComponent::GetComponentToWorldMatrix() const -> FMatrix
	{
		return ComponentToWorld.ToMatrix();
	}

	auto DSceneComponent::OnUpdateTransform() -> void
	{
	}

	auto DSceneComponent::CanAttachTo(const DSceneComponent* Parent) const -> bool
	{
		if (!Parent || Parent == this || Parent->IsAttachedTo(this))
		{
			return false;
		}

		const AActor* Owner = GetOwner();
		const AActor* ParentOwner = Parent->GetOwner();
		if ((Owner == nullptr) != (ParentOwner == nullptr))
		{
			return false;
		}
		return !Owner || Owner->GetOuter() == ParentOwner->GetOuter();
	}

	auto DSceneComponent::RemoveAttachChild(DSceneComponent* Child) -> void
	{
		std::erase_if(AttachChildren, [Child](const TObjectPtr<DSceneComponent>& Entry) { return Entry.Get() == Child; });
	}
}
