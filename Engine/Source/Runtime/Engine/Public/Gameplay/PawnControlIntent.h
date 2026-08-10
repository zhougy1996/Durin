#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "PawnControlIntent.gen.h"

namespace Durin
{
	// Carries one bounded, source-neutral control sample from a controller to a pawn.
	DSTRUCT()
	struct FPawnControlIntent
	{
		GENERATED_BODY()

		DPROPERTY()
		FVector2 Move{0.0};

		DPROPERTY()
		FVector2 Look{0.0};

		DPROPERTY()
		bool bJumpHeld = false;

		DPROPERTY()
		bool bJumpPressed = false;

		DPROPERTY()
		bool bJumpReleased = false;

		auto operator==(const FPawnControlIntent&) const -> bool = default;
	};
}
