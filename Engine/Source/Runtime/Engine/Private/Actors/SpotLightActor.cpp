#include "Actors/SpotLightActor.h"

#include "Components/SpotLightComponent.h"

namespace Durin
{
	ASpotLightActor::ASpotLightActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		LightComponent = CreateDefaultComponent<DSpotLightComponent>("SpotLightComponent");
		SetRootComponent(LightComponent);
	}

	auto ASpotLightActor::GetLightComponent() const -> DSpotLightComponent*
	{
		return LightComponent.Get();
	}
}
