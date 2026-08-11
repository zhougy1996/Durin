#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "Physics/PhysicsTypes.h"

#include "BodySetup.gen.h"

namespace Durin
{
	// Identifies the authored simple geometry published by a body setup.
	DENUM()
	enum class EBodySetupShapeType : uint8
	{
		None,
		Box,
		Sphere,
		Capsule
	};

	// Owns reusable asset collision independently from render data and component transforms.
	DCLASS()
	class DBodySetup : public DObject
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DBodySetup(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto SetBox(const FVector3& HalfExtent, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto SetSphere(double Radius, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto SetCapsule(double Radius, double HalfHeight, const FVector3& Center = FVector3(0.0)) -> bool;
		ENGINE_API auto BuildShape(FCollisionShape& OutShape, FTransform& OutLocalTransform) const -> bool;
		ENGINE_API auto BuildGeometry(FCollisionGeometryRef& OutGeometry, FTransform& OutLocalTransform) const -> bool;
		ENGINE_API auto IsValid(std::string* OutDiagnostic = nullptr) const -> bool;
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetShapeType() const -> EBodySetupShapeType { return ShapeType; }

	private:
		DPROPERTY()
		EBodySetupShapeType ShapeType = EBodySetupShapeType::None;

		// Box uses XYZ half extents; Sphere uses X radius; Capsule uses X radius and Z half height.
		DPROPERTY(Edit)
		FVector3 Dimensions{0.0};

		DPROPERTY(Edit)
		FVector3 Center{0.0};

		DPROPERTY()
		uint64 Revision = 1;

		mutable FCollisionGeometryRef CachedGeometry;
		mutable uint64 CachedGeometryRevision = 0;
	};
}
