#pragma once

#include "Engine/Actor.h"

#include "SkyBoxActor.gen.h"

namespace Durin
{
	class DSkyBoxComponent;

	// Provides an actor-owned skybox component for level placement.
	DCLASS(DisplayName = "Sky Box Actor", DefaultObjectName = "SkyBoxActor")
	class ASkyBoxActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit ASkyBoxActor(const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetSkyBoxComponent() const -> DSkyBoxComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DSkyBoxComponent> SkyBoxComponent;
	};
}
