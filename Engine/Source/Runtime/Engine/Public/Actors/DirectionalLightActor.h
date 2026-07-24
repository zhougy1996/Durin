#pragma once

#include "Engine/Actor.h"

#include "DirectionalLightActor.gen.h"

namespace Durin
{
	class DDirectionalLightComponent;

	// Provides an actor-owned directional light component for scene lighting.
	DCLASS()
	class ADirectionalLightActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit ADirectionalLightActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetLightComponent() const -> DDirectionalLightComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DDirectionalLightComponent> LightComponent;
	};
}
