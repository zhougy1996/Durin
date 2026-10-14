#pragma once

#include "CoreMinimal.h"

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
		// Cancellation returns empty data and remains recorded in Context.Cancellation.
		virtual auto Execute(const FAssetBuildTaskContext& Context) const
			-> std::expected<FTriMeshCollisionData, std::string> = 0;
	};
}
