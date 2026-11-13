#pragma once

#include "CoreMinimal.h"
#include "WaterAPI.h"
#include "WaterSurfaceTypes.h"

namespace Durin
{
	struct FStaticMeshRenderData;
	WATER_API auto MakeWaterGrid(const FWaterSurfaceSettings& Settings) -> std::unique_ptr<FStaticMeshRenderData>;
}
