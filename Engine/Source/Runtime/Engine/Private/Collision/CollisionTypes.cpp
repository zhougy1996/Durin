#include "Collision/CollisionTypes.h"

namespace Durin
{
	FCollisionResponseContainer::FCollisionResponseContainer()
		: FCollisionResponseContainer(ECollisionResponse::Block)
	{
	}

	FCollisionResponseContainer::FCollisionResponseContainer(ECollisionResponse DefaultResponse)
	{
		Responses.fill(DefaultResponse);
	}

	auto FCollisionResponseContainer::SetResponse(
		ECollisionChannel Channel, ECollisionResponse Response) -> void
	{
		Responses[static_cast<uint8>(Channel)] = Response;
	}

	auto FCollisionQueryParams::AddIgnoredActor(const AActor* Actor) -> void
	{
		if (Actor && std::ranges::find(IgnoredActors, Actor) == IgnoredActors.end()) IgnoredActors.push_back(Actor);
	}

	auto FCollisionQueryParams::AddIgnoredComponent(const DPrimitiveComponent* Component) -> void
	{
		if (Component && std::ranges::find(IgnoredComponents, Component) == IgnoredComponents.end())
			IgnoredComponents.push_back(Component);
	}

	auto FCollisionObjectQueryParams::AddObjectType(ECollisionChannel Channel) -> void
	{
		ObjectChannels[static_cast<uint8>(Channel)] = true;
	}

	auto ToPhysicsResponse(ECollisionResponse Response) -> EPhysicsQueryResponse
	{
		switch (Response)
		{
		case ECollisionResponse::Ignore: return EPhysicsQueryResponse::Ignore;
		case ECollisionResponse::Overlap: return EPhysicsQueryResponse::Overlap;
		case ECollisionResponse::Block: return EPhysicsQueryResponse::Block;
		}
		return EPhysicsQueryResponse::Ignore;
	}

	namespace CollisionProfile
	{
		auto Resolve(FName ProfileName, FProfile& OutProfile) -> bool
		{
			OutProfile = {};
			if (ProfileName == NoCollision) return true;
			if (ProfileName == BlockAll)
			{
				OutProfile.Enabled = ECollisionEnabled::QueryAndPhysics;
				return true;
			}
			if (ProfileName == WorldStatic)
			{
				OutProfile.Enabled = ECollisionEnabled::QueryAndPhysics;
				OutProfile.ObjectChannel = ECollisionChannel::WorldStatic;
				return true;
			}
			if (ProfileName == Pawn)
			{
				OutProfile.Enabled = ECollisionEnabled::QueryOnly;
				OutProfile.ObjectChannel = ECollisionChannel::Pawn;
				return true;
			}
			if (ProfileName == Trigger)
			{
				OutProfile.Enabled = ECollisionEnabled::QueryOnly;
				OutProfile.Responses = FCollisionResponseContainer(ECollisionResponse::Overlap);
				return true;
			}
			return false;
		}
	}
}
