#include "Actors/TerrainActor.h"

#include "Components/TerrainComponent.h"

namespace Durin
{
	ATerrainActor::ATerrainActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		TerrainComponent = CreateDefaultComponent<DTerrainComponent>("TerrainComponent");
		SetRootComponent(TerrainComponent);
	}

	auto ATerrainActor::GetTerrainComponent() const -> DTerrainComponent*
	{
		return TerrainComponent.Get();
	}
} // namespace Durin
