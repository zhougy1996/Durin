#pragma once
#include "EngineAPI.h"

namespace Durin
{
	// Explicit editor/tool bootstrap after TextureBuild and MeshBuilder are loaded.
	// Game builds do not initialize authoring providers.
	ENGINE_API auto InitializeAssetBuildService() -> bool;
	// Call from the owning shutdown thread after asset compilation closes and before
	// provider unload. Must not be called from a build callback/execution stack.
	ENGINE_API auto ShutdownAssetBuildService() -> void;
}
