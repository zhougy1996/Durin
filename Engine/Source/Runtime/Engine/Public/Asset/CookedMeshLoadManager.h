#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"

namespace Durin
{
	ENGINE_API auto InitializeCookedMeshLoadManager() -> bool;
	ENGINE_API auto PumpCookedMeshLoadManager() -> uint32;
	ENGINE_API auto ShutdownCookedMeshLoadManager() -> void;
}
