#include "Physics/PhysicsTypes.h"

namespace Durin
{
	auto IsValidPhysicsTransform(const FTransform& Transform) -> bool
	{
		FQuat NormalizedRotation;
		return Math::IsFinite(Transform.Translation)
			&& Math::IsFinite(Transform.Scale3D)
			&& Transform.Scale3D.x > 0.0
			&& Transform.Scale3D.y > 0.0
			&& Transform.Scale3D.z > 0.0
			&& Math::TryNormalize(Transform.Rotation, NormalizedRotation);
	}
}
