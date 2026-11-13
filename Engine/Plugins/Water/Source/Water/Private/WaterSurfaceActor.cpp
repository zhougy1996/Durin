#include "WaterSurfaceActor.h"
#include "WaterSurfaceComponent.h"

namespace Durin
{
	AWaterSurfaceActor::AWaterSurfaceActor(const FObjectInitializer& Initializer) : Super(Initializer)
	{
		WaterComponent = CreateDefaultComponent<DWaterSurfaceComponent>("WaterSurface");
		SetRootComponent(WaterComponent);
	}
	auto AWaterSurfaceActor::GetWaterComponent() const -> DWaterSurfaceComponent* { return WaterComponent.Get(); }
}
