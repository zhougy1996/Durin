#pragma once

#include "EngineAPI.h"
#include "Engine/Actor.h"

#include "TerrainActor.gen.h"

namespace Durin
{
	class DTerrainComponent;

	// Provides an actor-owned terrain component for finite heightfield geometry.
	DCLASS(DisplayName = "Terrain Actor", DefaultObjectName = "TerrainActor")
	class ATerrainActor final : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit ATerrainActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetTerrainComponent() const -> DTerrainComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DTerrainComponent> TerrainComponent;
	};
} // namespace Durin
