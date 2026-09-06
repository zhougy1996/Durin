#pragma once

#include "EngineAPI.h"
#include "DObject/Object.h"
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

	DENUM()
	enum class EBodySetupCollisionSourceMode : uint8
	{
		None = 0,
		ConvexHullFromLOD0 = 1,
		TriangleMeshFromLOD0 = 2
	};

	DENUM()
	enum class EBodySetupCollisionQueryPolicy : uint8
	{
		SimpleOnly,
		ComplexOnly,
		SimpleAndComplex
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
		ENGINE_API auto BuildSimpleGeometry(FCollisionGeometryRef& OutGeometry) const -> bool;
		ENGINE_API auto BuildComplexGeometry(FCollisionGeometryRef& OutGeometry) const -> bool;
		ENGINE_API auto SetCollisionSourceMode(EBodySetupCollisionSourceMode Mode) -> bool;
		ENGINE_API auto SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy Policy) -> bool;
		// Installs compatible immutable geometry and advances its revision.
		// Build/cache provenance is deliberately not retained by the physics owner.
		ENGINE_API auto SetCollisionGeometry(
			const FCollisionGeometryRef& Simple,
			const FCollisionGeometryRef& Complex) -> bool;
		ENGINE_API auto ClearCollisionGeometry() -> void;
		ENGINE_API auto IsValid(std::string* OutDiagnostic = nullptr) const -> bool;
		// Borrow installed geometry without constructing or refreshing primitive caches.
		auto GetResidentSimpleGeometry() const -> const FCollisionGeometryRef& { return CachedSimpleCollision; }
		auto GetResidentComplexGeometry() const -> const FCollisionGeometryRef& { return CachedComplexCollision; }
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetDimensions() const -> FVector3 { return Dimensions; }
		auto GetCenter() const -> FVector3 { return Center; }
		auto GetShapeType() const -> EBodySetupShapeType { return ShapeType; }
		auto GetCollisionSourceMode() const -> EBodySetupCollisionSourceMode { return CollisionSourceMode; }
		auto GetCollisionQueryPolicy() const -> EBodySetupCollisionQueryPolicy { return CollisionQueryPolicy; }
		auto GetCollisionBuildRevision() const -> uint64 { return CollisionBuildRevision; }

	private:
		DPROPERTY()
		EBodySetupShapeType ShapeType = EBodySetupShapeType::None;

		// Box uses XYZ half extents; Sphere uses X radius; Capsule uses X radius and Z half height.
		DPROPERTY(Edit, Category = "Shape", ToolTip = "Shape dimensions in local space", Units = "Meters")
		FVector3 Dimensions{0.0};

		DPROPERTY(Edit)
		FVector3 Center{0.0};

		DPROPERTY()
		uint64 Revision = 1;

		DPROPERTY(Edit)
		EBodySetupCollisionSourceMode CollisionSourceMode = EBodySetupCollisionSourceMode::None;

		DPROPERTY(Edit)
		EBodySetupCollisionQueryPolicy CollisionQueryPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;

		DPROPERTY(EditorOnly)
		uint64 CollisionBuildRevision = 0;

		mutable FCollisionGeometryRef CachedGeometry;
		mutable uint64 CachedGeometryRevision = 0;
		FCollisionGeometryRef CachedSimpleCollision;
		FCollisionGeometryRef CachedComplexCollision;
	};
}
