#pragma once

#include "EngineAPI.h"

namespace Durin
{
	// Registers Engine-owned asset policies once, independently of object construction.
	ENGINE_API auto InitializeEngineAssetServices() -> void;
	// Pumps at most 64 completions per Engine-owned asset service on the GameThread.
	// The pump remains available until ShutdownEngineAssetServices drains each owned service.
	ENGINE_API auto PumpEngineAssetServiceCompletions() -> void;
	// Starts one diagnostic asynchronous result that must complete through the normal frame pump.
	ENGINE_API auto BeginEngineAssetServiceLifecycleSmoke() -> void;
	// Requires the diagnostic result to have been discarded exactly once on the GameThread.
	ENGINE_API auto ValidateEngineAssetServiceLifecycleSmoke() -> void;
	// Stops Engine-owned asset workers before the process task system closes.
	ENGINE_API auto ShutdownEngineAssetServices() -> void;
}
