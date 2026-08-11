#pragma once

#include "Collision/CollisionTypes.h"

#include "BodyInstance.gen.h"

namespace Durin
{
	// Owns editable per-component collision settings and the transient handle for one scene body.
	DSTRUCT()
	struct FBodyInstance
	{
		GENERATED_BODY()

		DPROPERTY(Edit)
		ECollisionEnabled CollisionEnabled = ECollisionEnabled::NoCollision;

		DPROPERTY(Edit)
		ECollisionChannel ObjectChannel = ECollisionChannel::WorldDynamic;

		FCollisionResponseContainer Responses;

		DPROPERTY(Edit)
		FName ProfileName = CollisionProfile::NoCollision;

		FPhysicsActorHandle ActorHandle;
		uint64 PublishedBodySetupRevision = 0;
	};
}
