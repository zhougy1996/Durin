#pragma once

#include "Physics/CollisionDataProvider.h"
#include "Physics/BodySetupTypes.h"

namespace Durin
{
	// Pure owning cook input. Geometry preparation has already completed.
	struct FCookBodySetupInfo
	{
		FTriMeshCollisionData TriangleMeshDesc;
		EBodySetupCollisionSourceMode Mode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		bool bPersistDerivedData = true;
	};
}
