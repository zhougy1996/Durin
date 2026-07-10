#include "Actors/CameraActor.h"

#include "Components/CameraComponent.h"

namespace Durin
{
	ACameraActor::ACameraActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		CameraComponent = CreateDefaultComponent<DCameraComponent>("DCameraComponent");
		SetRootComponent(CameraComponent);
	}

	auto ACameraActor::GetCameraComponent() const -> DCameraComponent*
	{
		return CameraComponent.Get();
	}
} // namespace Durin
