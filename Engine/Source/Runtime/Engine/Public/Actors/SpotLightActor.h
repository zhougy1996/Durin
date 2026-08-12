#pragma once

#include "Engine/Actor.h"

#include "SpotLightActor.gen.h"

namespace Durin
{
	class DSpotLightComponent;

	// Provides an actor-owned spot-light serialization root.
	DCLASS()
	class ASpotLightActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit ASpotLightActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetLightComponent() const -> DSpotLightComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DSpotLightComponent> LightComponent;
	};
}
