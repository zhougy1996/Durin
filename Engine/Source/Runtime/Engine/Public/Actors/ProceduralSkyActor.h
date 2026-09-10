#pragma once

#include "Engine/Actor.h"
#include "ProceduralSkyActor.gen.h"

namespace Durin
{
	class DProceduralSkyComponent;
	// Scene placement for the shared analytic sky provider.
	DCLASS(DisplayName = "Procedural Sky", DefaultObjectName = "ProceduralSky")
	class AProceduralSkyActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit AProceduralSkyActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetSkyComponent() const -> DProceduralSkyComponent*;
	private:
		DPROPERTY()
		TObjectPtr<DProceduralSkyComponent> SkyComponent;
	};
}
