#pragma once

#include "Components/ActorComponent.h"
#include "Gameplay/PawnControlIntent.h"

#include "PawnMovementComponent.gen.h"

namespace Durin
{
	class APawn;

	// Defines the abstract pawn-owned velocity and semantic movement-update boundary.
	DCLASS(Abstract)
	class DPawnMovementComponent : public DActorComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DPawnMovementComponent(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetPawnOwner() const -> APawn*;
		auto GetVelocity() const -> const FVector3& { return Velocity; }
		auto SetVelocity(const FVector3& InVelocity) -> void { Velocity = InVelocity; }

	protected:
		// Concrete project components decide whether and how the intent changes movement or transform.
		virtual auto PerformMovement(const FPawnControlIntent& Intent, float DeltaSeconds) -> void = 0;

	private:
		DPROPERTY(Transient)
		FVector3 Velocity{0.0};

		friend class APawn;
	};
}
