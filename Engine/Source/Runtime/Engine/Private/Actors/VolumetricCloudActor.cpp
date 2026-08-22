#include "Actors/VolumetricCloudActor.h"

#include "Components/VolumetricCloudComponent.h"

namespace Durin
{
	AVolumetricCloudActor::AVolumetricCloudActor(
		const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		VolumetricCloudComponent =
			CreateDefaultComponent<DVolumetricCloudComponent>("VolumetricCloudComponent");
		SetRootComponent(VolumetricCloudComponent);
	}

	auto AVolumetricCloudActor::GetVolumetricCloudComponent() const
		-> DVolumetricCloudComponent*
	{
		return VolumetricCloudComponent.Get();
	}
}
