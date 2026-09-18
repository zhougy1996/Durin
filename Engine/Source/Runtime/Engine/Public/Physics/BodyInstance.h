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

		DPROPERTY(Edit, MetaData="DefaultCollapsed")
		FCollisionResponseContainer Responses;

		DPROPERTY(Edit)
		FName CollisionProfileName = CollisionProfile::NoCollision;

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

	class FBodyInstanceValidationCause final : public IObjectValidationCause
	{
	public:
		std::string CollisionProfileName;
		auto Format() const -> std::string override { return "Unknown collision profile."; }
	};

	// All archive consumers restore the same preset/custom semantics before publication.
	template<>
	struct TDStructOpsTraits<FBodyInstance> : TDStructOpsTraitsBase<FBodyInstance>
	{
		static constexpr bool bWithPostDeserialize = true;

		static auto PostDeserialize(FBodyInstance& Value, FDStructPostDeserializeContext&) -> FObjectValidationResult
		{
			if (Value.LoadProfileData()) return {};
			auto Cause = std::make_shared<FBodyInstanceValidationCause>();
			Cause->CollisionProfileName = Value.CollisionProfileName.ToString();
			return {{.Code = EObjectValidationError::StructRejected, .Cause = std::move(Cause)}};
		}
	};
}
