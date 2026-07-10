#include "Components/SceneComponent.h"

#include "Engine/Actor.h"

namespace Durin
{
	auto DSceneComponent::BeginDestroy() -> void
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
		Super::BeginDestroy();
	}

	auto DSceneComponent::GetRelativeTransform() const -> const FTransform&
	{
		return RelativeTransform;
	}

	auto DSceneComponent::SetRelativeTransform(const FTransform& InTransform) -> void
	{
		RelativeTransform = InTransform;
		RelativeTransform.Rotation = glm::normalize(RelativeTransform.Rotation);
		UpdateComponentToWorld();
	}

	auto DSceneComponent::GetWorldTransform() const -> const FTransform&
	{
		return ComponentToWorld;
	}

	auto DSceneComponent::SetWorldTransform(const FTransform& InTransform) -> void
	{
		if (DSceneComponent* Parent = AttachParent.Get())
		{
			RelativeTransform = FTransform::MakeRelative(InTransform, Parent->GetWorldTransform());
		}
		else
		{
			RelativeTransform = InTransform;
		}
		RelativeTransform.Rotation = glm::normalize(RelativeTransform.Rotation);
		UpdateComponentToWorld();
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
		return true;
	}

	auto DSceneComponent::DetachFromComponent(EDetachmentTransformRule Rule) -> bool
	{
		DSceneComponent* Parent = AttachParent.Get();
		if (!Parent)
		{
			return false;
		}

		const FTransform PreviousWorld = ComponentToWorld;
		Parent->RemoveAttachChild(this);
		AttachParent = nullptr;
		if (Rule == EDetachmentTransformRule::KeepWorld)
		{
			RelativeTransform = PreviousWorld;
		}
		UpdateComponentToWorld();
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
