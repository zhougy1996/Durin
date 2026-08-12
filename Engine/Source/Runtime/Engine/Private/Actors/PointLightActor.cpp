#include "Actors/PointLightActor.h"

#include "Components/PointLightComponent.h"

namespace Durin
{
	APointLightActor::APointLightActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		LightComponent = CreateDefaultComponent<DPointLightComponent>("PointLightComponent");
		SetRootComponent(LightComponent);
	}

	auto APointLightActor::GetLightComponent() const -> DPointLightComponent*
	{
		return LightComponent.Get();
	}
}
