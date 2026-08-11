#pragma once

#include "AetherCoreAPI.h"
#include "Collision/CollisionShape.h"
#include "Math/Transform.h"

namespace Durin
{
	inline constexpr uint8 MaximumPhysicsChannels = 32;

	// Opaque identity for one body registration in one physics scene.
	struct FPhysicsActorHandle
	{
		uint64 Id = 0;
		uint32 Generation = 0;

		auto IsValid() const -> bool { return Id != 0 && Generation != 0; }
		auto operator<=>(const FPhysicsActorHandle&) const = default;
	};

	// Low-level response used only at the Aether scene/query boundary.
	enum class EPhysicsQueryResponse : uint8
	{
		Ignore,
		Overlap,
		Block
	};

	// Classifies scene bodies by spatial-mutation policy without implying simulation behavior.
	enum class EPhysicsBodyMotionType : uint8
	{
		Static,
		Kinematic,
		Dynamic
	};

	// Carries object-channel and per-query-channel response values without Engine types.
	struct FPhysicsFilterData
	{
		uint8 ObjectChannel = 0;
		std::array<EPhysicsQueryResponse, MaximumPhysicsChannels> Responses{};

		FPhysicsFilterData()
		{
			Responses.fill(EPhysicsQueryResponse::Block);
		}
	};

	// Describes one immutable body publication to an FPhysicsScene.
	struct FPhysicsBodyDesc
	{
		FCollisionShape Shape;
		FTransform Transform;
		FPhysicsFilterData Filter;
		EPhysicsBodyMotionType MotionType = EPhysicsBodyMotionType::Kinematic;
		uint64 UserToken = 0;
	};

	// Value-type result returned by low-level scene queries.
	struct FPhysicsQueryHit
	{
		FPhysicsActorHandle ActorHandle;
		EPhysicsQueryResponse Response = EPhysicsQueryResponse::Ignore;
		double Time = 1.0;
		double Distance = 0.0;
		FVector3 Location{0.0};
		FVector3 ImpactPoint{0.0};
		FVector3 ImpactNormal{0.0};
		double PenetrationDepth = 0.0;
		uint64 UserToken = 0;
		bool bStartPenetrating = false;

		auto IsHit() const -> bool { return ActorHandle.IsValid(); }
	};

	// Low-level query filter copied by value for synchronous query execution.
	struct FPhysicsQueryFilter
	{
		uint8 QueryChannel = 0;
		std::array<EPhysicsQueryResponse, MaximumPhysicsChannels> Responses{};
		std::vector<FPhysicsActorHandle> IgnoredActors;

		FPhysicsQueryFilter()
		{
			Responses.fill(EPhysicsQueryResponse::Block);
		}
	};

	AETHERCORE_API auto IsValidPhysicsTransform(const FTransform& Transform) -> bool;
}
