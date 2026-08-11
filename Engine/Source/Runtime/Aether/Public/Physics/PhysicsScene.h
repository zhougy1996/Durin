#pragma once

#include "AetherAPI.h"
#include "Physics/PhysicsTypes.h"

namespace Durin
{
	// Immutable debug copy of one low-level scene entry.
	struct FPhysicsBodySnapshot
	{
		FPhysicsActorHandle Handle;
		FPhysicsBodyDesc Desc;
	};

	// Owns deterministic query-only body state for one World without Engine object pointers.
	class FPhysicsScene
	{
	public:
		AETHER_API FPhysicsScene();
		AETHER_API ~FPhysicsScene();
		FPhysicsScene(const FPhysicsScene&) = delete;
		auto operator=(const FPhysicsScene&) -> FPhysicsScene& = delete;

		AETHER_API auto AddBody(const FPhysicsBodyDesc& Desc) -> FPhysicsActorHandle;
		AETHER_API auto RemoveBody(FPhysicsActorHandle Handle) -> bool;
		AETHER_API auto UpdateBody(FPhysicsActorHandle Handle, const FPhysicsBodyDesc& Desc) -> bool;
		AETHER_API auto ContainsBody(FPhysicsActorHandle Handle) const -> bool;
		AETHER_API auto GetBodyCount() const -> size_t;
		AETHER_API auto CaptureBodies() const -> std::vector<FPhysicsBodySnapshot>;

		AETHER_API auto LineTraceSingle(
			const FVector3& Start,
			const FVector3& End,
			const FPhysicsQueryFilter& Filter,
			FPhysicsQueryHit& OutHit) const -> bool;
		AETHER_API auto SweepSingle(
			const FCollisionShape& Shape,
			const FTransform& StartTransform,
			const FVector3& Delta,
			const FPhysicsQueryFilter& Filter,
			FPhysicsQueryHit& OutHit) const -> bool;
		AETHER_API auto OverlapMulti(
			const FCollisionShape& Shape,
			const FTransform& Transform,
			const FPhysicsQueryFilter& Filter,
			std::vector<FPhysicsQueryHit>& OutHits) const -> bool;

	private:
		struct FBodyRecord
		{
			FPhysicsActorHandle Handle;
			FPhysicsBodyDesc Desc;
		};

		auto IsOwningThread() const -> bool;
		auto FindBody(FPhysicsActorHandle Handle) -> FBodyRecord*;
		auto FindBody(FPhysicsActorHandle Handle) const -> const FBodyRecord*;

		std::thread::id OwningThread;
		std::vector<FBodyRecord> Bodies;
		uint64 NextHandleId = 1;
	};
}
