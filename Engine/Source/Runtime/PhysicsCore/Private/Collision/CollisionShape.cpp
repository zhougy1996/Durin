#include "Collision/CollisionShape.h"

namespace Durin
{
	auto FCollisionShape::MakeBox(const FVector3& HalfExtent) -> FCollisionShape
	{
		FCollisionShape Result;
		Result.Type = ECollisionShapeType::Box;
		Result.Dimensions = HalfExtent;
		return Result;
	}

	auto FCollisionShape::MakeSphere(double Radius) -> FCollisionShape
	{
		FCollisionShape Result;
		Result.Type = ECollisionShapeType::Sphere;
		Result.Dimensions = FVector3(Radius);
		return Result;
	}

	auto FCollisionShape::MakeCapsule(double Radius, double HalfHeight) -> FCollisionShape
	{
		FCollisionShape Result;
		Result.Type = ECollisionShapeType::Capsule;
		Result.Dimensions = FVector3(Radius, Radius, HalfHeight);
		return Result;
	}

	auto FCollisionShape::IsValid() const -> bool
	{
		if (!Math::IsFinite(Dimensions)) return false;
		switch (Type)
		{
		case ECollisionShapeType::Box:
			return Dimensions.x > 0.0 && Dimensions.y > 0.0 && Dimensions.z > 0.0;
		case ECollisionShapeType::Sphere:
			return Dimensions.x > 0.0;
		case ECollisionShapeType::Capsule:
			return Dimensions.x > 0.0 && Dimensions.z >= Dimensions.x;
		}
		return false;
	}
}
