#pragma once

#include "CoreMinimal.h"
#include "Engine/Actor.h"
#include "WaterAPI.h"
#include "WaterSurfaceActor.gen.h"

namespace Durin
{
	class DWaterSurfaceComponent;
	DCLASS(DisplayName = "Water Surface", DefaultObjectName = "WaterSurface")
	class AWaterSurfaceActor final : public AActor
	{
		GENERATED_BODY()
	public:
		WATER_API explicit AWaterSurfaceActor(const FObjectInitializer& Initializer);
		WATER_API auto GetWaterComponent() const -> DWaterSurfaceComponent*;
	private:
		DPROPERTY()
		TObjectPtr<DWaterSurfaceComponent> WaterComponent;
	};
}
