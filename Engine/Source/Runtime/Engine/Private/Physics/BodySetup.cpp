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
		if (CollisionSourceMode != EBodySetupCollisionSourceMode::None)
		{
			OutLocalTransform = FTransform();
			if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::ComplexOnly
				&& BuildSimpleGeometry(OutGeometry)) return true;
			if (CollisionQueryPolicy != EBodySetupCollisionQueryPolicy::SimpleOnly
				&& BuildComplexGeometry(OutGeometry)) return true;
			return false;
		}
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

	auto DBodySetup::BuildSimpleGeometry(FCollisionGeometryRef& OutGeometry) const -> bool
	{
		OutGeometry = CachedSimpleCollision;
		if (OutGeometry) return true;
		if (CollisionSourceMode != EBodySetupCollisionSourceMode::None) return false;
		FTransform LocalTransform;
		return BuildGeometry(OutGeometry, LocalTransform);
	}

	auto DBodySetup::BuildComplexGeometry(FCollisionGeometryRef& OutGeometry) const -> bool
	{
		OutGeometry = CachedComplexCollision;
		return OutGeometry.IsValid();
	}

	auto DBodySetup::SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> bool
	{
		if (Mode != EBodySetupCollisionSourceMode::None
			&& Mode != EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			&& Mode != EBodySetupCollisionSourceMode::TriangleMeshFromLOD0) return false;
		if (CollisionSourceMode == Mode) return true;
		CollisionSourceMode = Mode;
		CachedSimpleCollision = {};
		CachedComplexCollision = {};
		++Revision;
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> bool
	{
		if (Policy != EBodySetupCollisionQueryPolicy::SimpleOnly
			&& Policy != EBodySetupCollisionQueryPolicy::ComplexOnly
			&& Policy != EBodySetupCollisionQueryPolicy::SimpleAndComplex) return false;
		if (CollisionQueryPolicy == Policy) return true;
		CollisionQueryPolicy = Policy;
		++Revision;
		MarkPackageDirty();
		return true;
	}

	auto DBodySetup::SetCollisionGeometry(
		const FCollisionGeometryRef& Simple,
		const FCollisionGeometryRef& Complex) -> bool
	{
		if (CollisionSourceMode == EBodySetupCollisionSourceMode::None
			|| (CollisionSourceMode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
				&& (!Simple || Simple.GetKind() != ECollisionGeometryKind::ConvexHull))
			|| (CollisionSourceMode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0
				&& (!Complex || Complex.GetKind() != ECollisionGeometryKind::TriangleMesh))
			|| (Simple && Simple.GetKind() != ECollisionGeometryKind::ConvexHull)
			|| (Complex && Complex.GetKind() != ECollisionGeometryKind::TriangleMesh)) return false;
		CachedSimpleCollision = Simple;
		CachedComplexCollision = Complex;
		++CollisionBuildRevision;
		++Revision;
		return true;
	}

	auto DBodySetup::ClearCollisionGeometry() -> void
	{
		CachedSimpleCollision = {};
		CachedComplexCollision = {};
		++CollisionBuildRevision;
		++Revision;
	}

	auto DBodySetup::IsValid(std::string* OutDiagnostic) const -> bool
	{
		FCollisionShape Shape;
		FTransform Transform;
		const bool bValid = CollisionSourceMode != EBodySetupCollisionSourceMode::None
			? (CachedSimpleCollision.IsValid() || CachedComplexCollision.IsValid())
			: BuildShape(Shape, Transform);
		if (OutDiagnostic) *OutDiagnostic = bValid ? std::string{}
			: CollisionSourceMode != EBodySetupCollisionSourceMode::None
				? "Body setup has no published collision geometry."
				: "Body setup has no valid finite simple collision geometry.";
		return bValid;
	}
}
