#pragma once

#include "Engine/Actor.h"

#include "PointLightActor.gen.h"

namespace Durin
{
	class DPointLightComponent;

	// Provides an actor-owned point-light serialization root.
	DCLASS()
	class APointLightActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit APointLightActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetLightComponent() const -> DPointLightComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DPointLightComponent> LightComponent;
	};
}
