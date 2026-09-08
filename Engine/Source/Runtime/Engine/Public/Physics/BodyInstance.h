#pragma once

#include "Collision/CollisionTypes.h"
#include "DObject/StructOps.h"

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

		DPROPERTY(Edit)
		FCollisionResponseContainer Responses;

		DPROPERTY(Edit)
		FName CollisionProfileName = CollisionProfile::NoCollision;

		// Read only to migrate assets authored before CollisionProfileName existed.
		DPROPERTY(Deprecated)
		FName ProfileName_DEPRECATED;

		// Resolves named presets without dirtying an owner or publishing physics state.
		auto LoadProfileData() -> bool
		{
			if (CollisionProfileName.IsNone()) return true;
			CollisionProfile::FProfile Profile;
			if (!CollisionProfile::Resolve(CollisionProfileName, Profile)) return false;
			CollisionEnabled = Profile.Enabled;
			ObjectChannel = Profile.ObjectChannel;
			Responses = Profile.Responses;
			return true;
		}

		FPhysicsActorHandle ActorHandle;
		uint64 PublishedBodySetupRevision = 0;
		// Non-reflected spatial mutation policy published independently from collision filters.
		EPhysicsBodyMotionType MotionType = EPhysicsBodyMotionType::Kinematic;
	};

	// All archive consumers restore the same preset/custom semantics before publication.
	template<>
	struct TDStructOpsTraits<FBodyInstance> : TDStructOpsTraitsBase<FBodyInstance>
	{
		static constexpr bool bWithPostDeserialize = true;

		static auto PostDeserialize(FBodyInstance& Value, FDStructPostDeserializeContext& Context) -> bool
		{
			if (Context.WasDeprecatedPropertyLoaded(FName("ProfileName_DEPRECATED")))
				Value.CollisionProfileName = Value.ProfileName_DEPRECATED;
			Value.ProfileName_DEPRECATED = FName();
			if (!Value.LoadProfileData()) return Context.Fail("Unknown collision profile.");
			return true;
		}
	};
}
