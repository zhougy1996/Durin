#include "Components/ShapeComponent.h"

namespace Durin
{
	DBoxComponent::DBoxComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DBoxComponent::SetBoxHalfExtent(const FVector3& HalfExtent) -> bool
	{
		if (!FCollisionShape::MakeBox(HalfExtent).IsValid()) return false;
		BoxHalfExtent = HalfExtent;
		MarkPackageDirty();
		RecreatePhysicsState();
		return true;
	}

	auto DBoxComponent::BuildCollisionShape(
		FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool
	{
		OutShape = FCollisionShape::MakeBox(BoxHalfExtent);
		OutWorldTransform = GetWorldTransform();
		return OutShape.IsValid() && IsValidPhysicsTransform(OutWorldTransform);
	}

	DSphereComponent::DSphereComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DSphereComponent::BuildCollisionShape(
		FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool
	{
		OutShape = FCollisionShape::MakeSphere(SphereRadius);
		OutWorldTransform = GetWorldTransform();
		return OutShape.IsValid() && IsValidPhysicsTransform(OutWorldTransform);
	}

	DCapsuleComponent::DCapsuleComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DCapsuleComponent::SetCapsuleSize(double Radius, double HalfHeight) -> bool
	{
		if (!FCollisionShape::MakeCapsule(Radius, HalfHeight).IsValid()) return false;
		CapsuleRadius = Radius;
		CapsuleHalfHeight = HalfHeight;
		MarkPackageDirty();
		RecreatePhysicsState();
		return true;
	}

	auto DCapsuleComponent::BuildCollisionShape(
		FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool
	{
		OutShape = FCollisionShape::MakeCapsule(CapsuleRadius, CapsuleHalfHeight);
		OutWorldTransform = GetWorldTransform();
		return OutShape.IsValid() && IsValidPhysicsTransform(OutWorldTransform);
	}
}
