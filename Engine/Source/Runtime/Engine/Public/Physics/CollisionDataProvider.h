#pragma once

#include "EngineAPI.h"
#include "Physics/PhysicsCookFailure.h"
#include <expected>
#include <memory>

namespace Durin
{
	struct FTriMeshCollisionData
	{
		std::vector<FVector3f> Positions;
		std::vector<uint32> Indices;
	};

	class FPhysicsMeshInputTask;
	class IInterface_CollisionDataProvider
	{
	public:
		virtual ~IInterface_CollisionDataProvider() = default;
		virtual auto ContainsPhysicsTriMeshData() const -> bool = 0;
		// Owner-thread call returning owned geometry, never deferred execution.
		virtual auto GetPhysicsTriMeshData() const
			-> std::expected<FTriMeshCollisionData, FPhysicsCookFailure> = 0;
		// Durin's asynchronous preparation extension. Default copies actual provider data;
		// source-backed assets override it to defer I/O and decoding on detached inputs.
		ENGINE_API virtual auto CreatePhysicsMeshInputTask() const
			-> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, FPhysicsCookFailure>;
	};
}
