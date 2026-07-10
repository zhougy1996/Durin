#include "Actors/CameraActor.h"

#include "Components/CameraComponent.h"

namespace Durin
{
	ACameraActor::ACameraActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		CameraComponent = CreateDefaultComponent<DCameraComponent>("DCameraComponent");
		RootComponent = CameraComponent;
	}

	auto ACameraActor::GetCameraComponent() const -> DCameraComponent*
	{
		return CameraComponent.Get();
	}
} // namespace Durin
