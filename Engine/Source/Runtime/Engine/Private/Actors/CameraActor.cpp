#include "Actors/CameraActor.h"

#include "Components/CameraComponent.h"

namespace Durin
{
	ACameraActor::ACameraActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		CameraComponent = NewObject<DCameraComponent>(this, "DCameraComponent");
		RootComponent = CameraComponent;
	}

	auto ACameraActor::GetCameraComponent() const -> DCameraComponent*
	{
		return CameraComponent.Get();
	}
} // namespace Durin
