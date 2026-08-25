#pragma once

#include "PhysicsCoreAPI.h"
#include "Math/Operations.h"

namespace Durin
{
	// Identifies the immutable simple geometry carried by a collision shape value.
	enum class ECollisionShapeType : uint8
	{
		Box,
		Sphere,
		Capsule
	};

	// Stores one Engine-independent simple collision shape in local space.
	class FCollisionShape
	{
	public:
		PHYSICSCORE_API static auto MakeBox(const FVector3& HalfExtent) -> FCollisionShape;
		PHYSICSCORE_API static auto MakeSphere(double Radius) -> FCollisionShape;
		// HalfHeight includes both hemispherical ends and must be at least Radius.
		PHYSICSCORE_API static auto MakeCapsule(double Radius, double HalfHeight) -> FCollisionShape;

		auto GetType() const -> ECollisionShapeType { return Type; }
		auto GetBoxHalfExtent() const -> const FVector3& { return Dimensions; }
		auto GetSphereRadius() const -> double { return Dimensions.x; }
		auto GetCapsuleRadius() const -> double { return Dimensions.x; }
		auto GetCapsuleHalfHeight() const -> double { return Dimensions.z; }
		PHYSICSCORE_API auto IsValid() const -> bool;

	private:
		ECollisionShapeType Type = ECollisionShapeType::Box;
		FVector3 Dimensions{0.0};
	};
}
