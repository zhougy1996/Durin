#pragma once

#include "Engine/Actor.h"
#include "SkyLightActor.gen.h"

namespace Durin
{
	class DSkyLightComponent;

	// Authored scene placement for an independent Sky Light.
	DCLASS(DisplayName = "Sky Light", DefaultObjectName = "SkyLight")
	class ASkyLightActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit ASkyLightActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetSkyLightComponent() const -> DSkyLightComponent*;
	private:
		DPROPERTY()
		TObjectPtr<DSkyLightComponent> SkyLightComponent;
	};
}
