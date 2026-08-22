#pragma once

#include "Engine/Actor.h"

#include "VolumetricCloudActor.gen.h"

namespace Durin
{
	class DVolumetricCloudComponent;

	DCLASS(DisplayName = "Volumetric Cloud Actor", DefaultObjectName = "VolumetricCloudActor")
	class AVolumetricCloudActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit AVolumetricCloudActor(
			const FObjectInitializer& ObjectInitializer);
		ENGINE_API auto GetVolumetricCloudComponent() const
			-> DVolumetricCloudComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DVolumetricCloudComponent> VolumetricCloudComponent;
	};
}
