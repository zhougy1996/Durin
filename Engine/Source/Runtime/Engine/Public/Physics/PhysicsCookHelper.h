#pragma once

#include "Asset/AssetBuildTaskContext.h"
#include "Physics/PhysicsCookVersion.h"
#include "Physics/CookBodySetupInfo.h"
#include "Collision/CollisionGeometry.h"

namespace Durin
{
	struct FPhysicsCookResult
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
	};

	class FPhysicsCookHelper
	{
	public:
		ENGINE_API static auto Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, FPhysicsCookFailure>;
	};
}
