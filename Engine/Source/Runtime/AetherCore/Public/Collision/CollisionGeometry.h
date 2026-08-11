#pragma once

#include "Collision/CollisionShape.h"
#include "Math/Transform.h"

namespace Durin
{
	struct FPhysicsQueryHit;

	// Describes one stable simple child supplied to immutable compound geometry creation.
	struct FCollisionGeometryChild
	{
		FCollisionShape Shape;
		FTransform LocalTransform;
	};

	class FCollisionGeometry;

	// Copyable owning reference to one validated immutable primitive or compound payload.
	class FCollisionGeometryRef
	{
	public:
		FCollisionGeometryRef() = default;
		AETHERCORE_API static auto MakePrimitive(const FCollisionShape& Shape) -> FCollisionGeometryRef;
		AETHERCORE_API static auto MakeCompound(std::span<const FCollisionGeometryChild> Children) -> FCollisionGeometryRef;

		auto IsValid() const -> bool { return Payload != nullptr; }
		explicit operator bool() const { return IsValid(); }
		AETHERCORE_API auto GetIdentity() const -> uint64;
		AETHERCORE_API auto GetChildCount() const -> uint32;
		AETHERCORE_API auto GetChild(uint32 Index) const -> const FCollisionGeometryChild*;
		AETHERCORE_API auto GetLocalBounds(FVector3& OutMin, FVector3& OutMax) const -> bool;
		AETHERCORE_API auto GetRetainedBytes() const -> uint64;

	private:
		explicit FCollisionGeometryRef(std::shared_ptr<const FCollisionGeometry> InPayload)
			: Payload(std::move(InPayload))
		{}

		std::shared_ptr<const FCollisionGeometry> Payload;
	};
}

namespace Durin::CollisionGeometry
{
	// Reports the complete internal outcome without changing scene bool query APIs.
	enum class ECollisionQueryStatus : uint8
	{
		Hit,
		Miss,
		Invalid,
		Unsupported,
		NonConverged
	};

	// Keeps the retained oracle distinct from bounded production selection.
	enum class ECollisionQueryAlgorithm : uint8
	{
		Reference,
		Production
	};

	// Optional zero-allocation sink for bounded reference-geometry work.
	struct FCollisionGeometryCounters
	{
		uint64 DistanceEvaluations = 0;
		uint64 SearchIterations = 0;
		uint64 LeafTests = 0;
		uint64 CompoundChildren = 0;
		uint64 AnalyticDispatches = 0;
		uint64 GenericDispatches = 0;
		uint64 SupportEvaluations = 0;
		uint64 NonConverged = 0;
		uint64 Unsupported = 0;
		uint64 ReferenceFallbacks = 0;
		bool bOverflowed = false;
	};

	AETHERCORE_API auto Raycast(
		const FVector3& Start,
		const FVector3& End,
		const FCollisionGeometryRef& Target,
		const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> ECollisionQueryStatus;

	AETHERCORE_API auto Sweep(
		const FCollisionShape& Query,
		const FTransform& QueryTransform,
		const FVector3& Delta,
		const FCollisionGeometryRef& Target,
		const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> ECollisionQueryStatus;

	AETHERCORE_API auto Overlap(
		const FCollisionShape& Query,
		const FTransform& QueryTransform,
		const FCollisionGeometryRef& Target,
		const FTransform& TargetTransform,
		ECollisionQueryAlgorithm Algorithm,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> ECollisionQueryStatus;

	// Traces a finite segment against a positive-scale oriented box.
	AETHERCORE_API auto RaycastBox(
		const FVector3& Start,
		const FVector3& End,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> bool;

	// Tests a capsule against a positive-scale oriented box and reports bounded penetration.
	AETHERCORE_API auto OverlapCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> bool;

	// Sweeps a capsule by Delta against a positive-scale oriented box.
	AETHERCORE_API auto SweepCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FVector3& Delta,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit,
		FCollisionGeometryCounters* Counters = nullptr) -> bool;
}
