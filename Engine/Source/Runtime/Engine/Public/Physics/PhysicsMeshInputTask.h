#pragma once

#include "Asset/AssetBuildTaskContext.h"
#include "Physics/CollisionDataProvider.h"

namespace Durin
{
	// Owns detached preparation inputs. No DObject/provider/render-resource references.
	// Execute is repeatable, with no owner-thread dependencies; the reservation covers preparation and cooking.
	class FPhysicsMeshInputTask
	{
	public:
		virtual ~FPhysicsMeshInputTask() = default;
		virtual auto GetWorkingSetBytes() const -> uint64 = 0;
		virtual auto Execute(const FAssetBuildTaskContext& Context) const
			-> std::expected<FTriMeshCollisionData, FPhysicsCookFailure> = 0;
	};
}
