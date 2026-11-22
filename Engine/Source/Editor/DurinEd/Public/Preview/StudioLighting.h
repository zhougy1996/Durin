#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin
{
    class DLevel;
    class DSkyLightComponent;
    // Authors a normal component reference. Used by new levels and preview scenes.
    DURINED_API auto AddStudioSkyLight(DLevel& Level) -> DSkyLightComponent*;
}
