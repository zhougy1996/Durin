#pragma once

#include "CoreMinimal.h"

namespace Durin::PhysicsCorePrivate
{
	// Bump for changes to triangle mesh, BVH or convex hull cooking results.
	inline constexpr uint32 CollisionCookAlgorithmRevision = 2;
}
