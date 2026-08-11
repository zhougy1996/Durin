#pragma once

#include "Components/PrimitiveComponent.h"

#include "ShapeComponent.gen.h"

namespace Durin
{
	// Provides editable analytic collision geometry without render-data ownership.
	DCLASS()
	class DShapeComponent : public DPrimitiveComponent
	{
		GENERATED_BODY()
	};

	// Publishes one component-local Box using positive half extents.
	DCLASS()
	class DBoxComponent : public DShapeComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DBoxComponent(const FObjectInitializer& ObjectInitializer);
		auto GetBoxHalfExtent() const -> const FVector3& { return BoxHalfExtent; }
		ENGINE_API auto SetBoxHalfExtent(const FVector3& HalfExtent) -> bool;
		ENGINE_API auto BuildCollisionShape(FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool override;

	private:
		DPROPERTY(Edit)
		FVector3 BoxHalfExtent{0.5};
	};

	// Publishes one component-local Sphere.
	DCLASS()
	class DSphereComponent : public DShapeComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSphereComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto BuildCollisionShape(FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool override;

	private:
		DPROPERTY(Edit)
		double SphereRadius = 0.5;
	};

	// Publishes one Z-axis Capsule whose half height includes its hemispherical ends.
	DCLASS()
	class DCapsuleComponent : public DShapeComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DCapsuleComponent(const FObjectInitializer& ObjectInitializer);
		auto GetCapsuleRadius() const -> double { return CapsuleRadius; }
		auto GetCapsuleHalfHeight() const -> double { return CapsuleHalfHeight; }
		ENGINE_API auto SetCapsuleSize(double Radius, double HalfHeight) -> bool;
		ENGINE_API auto BuildCollisionShape(FCollisionShape& OutShape, FTransform& OutWorldTransform) const -> bool override;

	private:
		DPROPERTY(Edit)
		double CapsuleRadius = 0.4;

		DPROPERTY(Edit)
		double CapsuleHalfHeight = 1.0;
	};
}
