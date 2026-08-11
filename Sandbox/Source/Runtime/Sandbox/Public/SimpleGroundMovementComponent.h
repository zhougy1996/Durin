#pragma once

#include "Components/PawnMovementComponent.h"
#include "SandboxAPI.h"

#include "SimpleGroundMovementComponent.gen.h"

namespace Durin::Sandbox
{
	// Moves the Sandbox pawn through bounded World capsule sweeps with one velocity authority.
	DCLASS()
	class DSimpleGroundMovementComponent final : public DPawnMovementComponent
	{
		GENERATED_BODY()
	public:
		SANDBOX_API explicit DSimpleGroundMovementComponent(const FObjectInitializer& ObjectInitializer);
		SANDBOX_API auto IsGrounded() const -> bool;

	protected:
		SANDBOX_API auto PerformMovement(const FPawnControlIntent& Intent, float DeltaSeconds) -> void override;
	};
}
