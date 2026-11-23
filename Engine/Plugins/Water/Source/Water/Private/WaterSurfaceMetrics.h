#pragma once

#include "CoreMinimal.h"
#include "WaterSurfaceTypes.h"

namespace Durin
{
	struct FWaterSurfaceMetrics
	{
		uint32 CellsX;
		uint32 CellsY;
		double VerticalEnvelope;
		double HorizontalEnvelope;
	};

	auto MeasureWaterSurface(const FResolvedWaterSurface& Settings) -> std::expected<FWaterSurfaceMetrics, std::string>;
}
