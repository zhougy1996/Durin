#include "Collision/CollisionTypes.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/Actor.h"
#include "Physics/PhysicsObjectID.h"

namespace Durin
{
	auto FHitResult::SetFromPhysicsHit(const FPhysicsQueryHit& Source) -> bool
	{
		auto* HitComponent = Cast<DPrimitiveComponent>(PhysicsPrivate::ResolveObjectID(Source.ComponentID));
		if (!HitComponent) { Reset(); return false; }
		bBlockingHit = Source.Response == EPhysicsQueryResponse::Block;
		bStartPenetrating = Source.bStartPenetrating;
		Time = Source.Time;
		Distance = Source.Distance;
		Location = Source.Location;
		ImpactPoint = Source.ImpactPoint;
		ImpactNormal = Source.ImpactNormal;
		PenetrationDepth = Source.PenetrationDepth;
		Component = HitComponent;
		Actor = Component->GetOwner();
		return true;
	}

	FCollisionResponseContainer::FCollisionResponseContainer()
		: FCollisionResponseContainer(ECollisionResponse::Block)
	{
	}

	FCollisionResponseContainer::FCollisionResponseContainer(ECollisionResponse DefaultResponse)
	{
		std::ranges::fill(Responses, DefaultResponse);
	}

	auto FCollisionResponseContainer::SetResponse(
		ECollisionChannel Channel, ECollisionResponse Response) -> void
	{
		Responses[static_cast<uint8>(Channel)] = Response;
	}

	namespace
	{
		auto AddIgnoredObject(std::vector<uint64>& IDs, const DObject* Object) -> void
		{
			const uint64 ID = PhysicsPrivate::MakeObjectID(Object);
			if (ID && !std::ranges::contains(IDs, ID)) IDs.push_back(ID);
		}
	}

	auto FCollisionQueryParams::AddIgnoredSourceObject(const DObject* SourceObject) -> void
	{
		AddIgnoredObject(IgnoredSourceObjectIDs, SourceObject);
	}

	auto FCollisionQueryParams::AddIgnoredActor(const AActor* Actor) -> void
	{
		AddIgnoredSourceObject(Actor);
	}

	auto FCollisionQueryParams::AddIgnoredComponent(const DPrimitiveComponent* Component) -> void
	{
		AddIgnoredObject(IgnoredComponentIDs, Component);
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
