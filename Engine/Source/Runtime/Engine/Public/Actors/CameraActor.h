#pragma once

#include "EngineAPI.h"
#include "Engine/Actor.h"

#include "CameraActor.gen.h"

namespace Durin
{
	class DCameraComponent;

	DCLASS()
	class ACameraActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API ACameraActor();
		ENGINE_API auto GetCameraComponent() const -> DCameraComponent*;

	private:
		DPROPERTY()
		TObjectPtr<DCameraComponent> CameraComponent;
	};
}
