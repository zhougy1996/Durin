#pragma once

#include "Engine/Actor.h"

#include "DirectionalLightActor.gen.h"

namespace Durin
{
	class DDirectionalLightComponent;

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
