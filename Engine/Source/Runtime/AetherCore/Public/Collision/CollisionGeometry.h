#pragma once

#include "Physics/PhysicsTypes.h"

namespace Durin::CollisionGeometry
{
	// Traces a finite segment against a positive-scale oriented box.
	AETHERCORE_API auto RaycastBox(
		const FVector3& Start,
		const FVector3& End,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit) -> bool;

	// Tests a capsule against a positive-scale oriented box and reports bounded penetration.
	AETHERCORE_API auto OverlapCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit) -> bool;

	// Sweeps a capsule by Delta against a positive-scale oriented box.
	AETHERCORE_API auto SweepCapsuleBox(
		const FCollisionShape& Capsule,
		const FTransform& CapsuleTransform,
		const FVector3& Delta,
		const FCollisionShape& Box,
		const FTransform& BoxTransform,
		FPhysicsQueryHit& OutHit) -> bool;
}
