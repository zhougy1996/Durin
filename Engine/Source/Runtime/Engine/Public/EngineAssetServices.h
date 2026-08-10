#pragma once

#include "EngineAPI.h"

namespace Durin
{
	// Registers Engine-owned asset policies once, independently of object construction.
	ENGINE_API auto InitializeEngineAssetServices() -> void;
	// Stops Engine-owned asset workers before the process task system closes.
	ENGINE_API auto ShutdownEngineAssetServices() -> void;
}
