#include "Components/PawnMovementComponent.h"

#include "Actors/Pawn.h"

namespace Durin
{
	DPawnMovementComponent::DPawnMovementComponent(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DPawnMovementComponent::GetPawnOwner() const -> APawn*
	{
		return Cast<APawn>(GetOwner());
	}
}
