#include "Actors/CameraActor.h"

#include "Components/CameraComponent.h"

namespace Durin
{
	ACameraActor::ACameraActor()
	{
		CameraComponent = NewObject<DCameraComponent>(this, "DCameraComponent");
		RootComponent = CameraComponent;
	}

	auto ACameraActor::GetCameraComponent() const -> DCameraComponent*
	{
		return CameraComponent.Get();
	}
}
