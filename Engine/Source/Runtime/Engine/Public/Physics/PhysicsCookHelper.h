#pragma once

#include "Asset/AssetBuildTaskContext.h"
#include "Physics/PhysicsCookVersion.h"
#include "Physics/CookBodySetupInfo.h"
#include "Collision/CollisionGeometry.h"
#include "Physics/PhysicsCookDiagnostics.h"

namespace Durin
{
	struct FPhysicsCookResult
	{
		FCollisionGeometryRef Simple;
		FCollisionGeometryRef Complex;
		auto GetCacheErrors() const -> const std::vector<FPhysicsCacheError>& { return CacheErrors; }
	private:
		std::vector<FPhysicsCacheError> CacheErrors;
		friend class FPhysicsCookHelper;
	};

	class FPhysicsCookHelper
	{
	public:
		ENGINE_API static auto Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Context = {})
			-> std::expected<FPhysicsCookResult, FPhysicsCookFailure>;
	};
}
