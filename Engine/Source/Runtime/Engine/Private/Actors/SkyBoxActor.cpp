#include "Actors/SkyBoxActor.h"

#include "Components/SkyBoxComponent.h"

namespace Durin
{
	ASkyBoxActor::ASkyBoxActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SkyBoxComponent = CreateDefaultComponent<DSkyBoxComponent>("SkyBoxComponent");
		SetRootComponent(SkyBoxComponent);
	}

	auto ASkyBoxActor::GetSkyBoxComponent() const -> DSkyBoxComponent*
	{
		return SkyBoxComponent.Get();
	}
}
