#include "Actors/SkyLightActor.h"
#include "Components/SkyLightComponent.h"

namespace Durin
{
	ASkyLightActor::ASkyLightActor(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
	{
		SkyLightComponent = CreateDefaultComponent<DSkyLightComponent>("SkyLightComponent");
		SetRootComponent(SkyLightComponent);
	}

	auto ASkyLightActor::GetSkyLightComponent() const -> DSkyLightComponent*
	{
		return SkyLightComponent.Get();
	}
}
