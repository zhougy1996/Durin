#pragma once

#include "Physics/PhysicsTypes.h"

namespace Durin::CollisionGeometry
{
	// Optional zero-allocation sink for bounded reference-geometry work.
	struct FCollisionGeometryCounters
	{
		uint64 DistanceEvaluations = 0;
		uint64 SearchIterations = 0;
		bool bOverflowed = false;
	};

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
