#include "Actors/DirectionalLightActor.h"

#include "Components/DirectionalLightComponent.h"

namespace Durin
{
	ADirectionalLightActor::ADirectionalLightActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		LightComponent = CreateDefaultComponent<DDirectionalLightComponent>("DirectionalLightComponent");
		SetRootComponent(LightComponent);
	}

	auto ADirectionalLightActor::GetLightComponent() const -> DDirectionalLightComponent*
	{
		return LightComponent.Get();
	}
}
