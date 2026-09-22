#pragma once

#include "Asset/AssetBuildTaskContext.h"
#include "Physics/PhysicsCookVersion.h"
#include "Physics/CookBodySetupInfo.h"
#include "Collision/CollisionGeometry.h"
#include "Asset/AssetBuildCacheWarning.h"

namespace Durin
{
	struct FPhysicsCookResult
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		auto GetCacheWarnings() const -> const std::vector<FAssetBuildCacheWarning>& { return CacheWarnings; }
	private:
		std::vector<FAssetBuildCacheWarning> CacheWarnings;
		friend class FPhysicsCookHelper;
	};

	class FPhysicsCookHelper
	{
	public:
		ENGINE_API static auto Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, FPhysicsCookFailure>;
	};
}
