#include "Actors/ProceduralSkyActor.h"
#include "Components/ProceduralSkyComponent.h"

namespace Durin
{
	AProceduralSkyActor::AProceduralSkyActor(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
	{
		SkyComponent = CreateDefaultComponent<DProceduralSkyComponent>("SkyComponent");
		SetRootComponent(SkyComponent);
	}
	auto AProceduralSkyActor::GetSkyComponent() const -> DProceduralSkyComponent* { return SkyComponent.Get(); }
}
