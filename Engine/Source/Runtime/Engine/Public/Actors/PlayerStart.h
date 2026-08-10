#pragma once

#include "Engine/Actor.h"

#include "PlayerStart.gen.h"

namespace Durin
{
	// Marks an authored transform as an eligible deterministic local-player spawn point.
	DCLASS()
	class APlayerStart : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit APlayerStart(const FObjectInitializer& ObjectInitializer);
	};
}
