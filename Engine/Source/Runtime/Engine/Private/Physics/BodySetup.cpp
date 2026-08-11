#include "Physics/BodySetup.h"

namespace Durin
{
	DBodySetup::DBodySetup(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

		auto DBodySetup::SetBox(const FVector3& HalfExtent, const FVector3& InCenter) -> bool
	{
		const FCollisionShape Candidate = FCollisionShape::MakeBox(HalfExtent);
		if (!Candidate.IsValid() || !Math::IsFinite(InCenter)) return false;
		ShapeType = EBodySetupShapeType::Box;
		Dimensions = HalfExtent;
		Center = InCenter;
		++Revision;
		CachedGeometry = {};
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetSphere(double Radius, const FVector3& InCenter) -> bool
	{
		const FCollisionShape Candidate = FCollisionShape::MakeSphere(Radius);
		if (!Candidate.IsValid() || !Math::IsFinite(InCenter)) return false;
		ShapeType = EBodySetupShapeType::Sphere;
		Dimensions = FVector3(Radius);
		Center = InCenter;
		++Revision;
		CachedGeometry = {};
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetCapsule(double Radius, double HalfHeight, const FVector3& InCenter) -> bool
	{
		const FCollisionShape Candidate = FCollisionShape::MakeCapsule(Radius, HalfHeight);
		if (!Candidate.IsValid() || !Math::IsFinite(InCenter)) return false;
		ShapeType = EBodySetupShapeType::Capsule;
		Dimensions = FVector3(Radius, Radius, HalfHeight);
		Center = InCenter;
		++Revision;
		CachedGeometry = {};
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::BuildShape(FCollisionShape& OutShape, FTransform& OutLocalTransform) const -> bool
	{
		switch (ShapeType)
		{
		case EBodySetupShapeType::Box: OutShape = FCollisionShape::MakeBox(Dimensions); break;
		case EBodySetupShapeType::Sphere: OutShape = FCollisionShape::MakeSphere(Dimensions.x); break;
		case EBodySetupShapeType::Capsule: OutShape = FCollisionShape::MakeCapsule(Dimensions.x, Dimensions.z); break;
		case EBodySetupShapeType::None: return false;
		}
		if (!OutShape.IsValid() || !Math::IsFinite(Center)) return false;
		OutLocalTransform = FTransform();
		OutLocalTransform.Translation = Center;
		return true;
	}

	auto DBodySetup::BuildGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutLocalTransform) const -> bool
	{
		FCollisionShape Shape;
		if (!BuildShape(Shape, OutLocalTransform)) return false;
		if (!CachedGeometry.IsValid() || CachedGeometryRevision != Revision)
		{
			CachedGeometry = FCollisionGeometryRef::MakePrimitive(Shape);
			CachedGeometryRevision = CachedGeometry.IsValid() ? Revision : 0;
		}
		OutGeometry = CachedGeometry;
		return OutGeometry.IsValid();
	}

	auto DBodySetup::IsValid(std::string* OutDiagnostic) const -> bool
	{
		FCollisionShape Shape;
		FTransform Transform;
		const bool bValid = BuildShape(Shape, Transform);
		if (OutDiagnostic) *OutDiagnostic = bValid ? std::string{} : "Body setup has no valid finite simple collision geometry.";
		return bValid;
	}
}
