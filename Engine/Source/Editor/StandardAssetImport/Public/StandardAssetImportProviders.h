#pragma once

#include "StandardAssetImportAPI.h"

namespace Durin::Asset::Import::Standard
{
	// Registers the built-in Scene, StaticMesh, Texture2D, and TextureCube
	// providers plus their exact-class imported-state handlers. Calls are idempotent.
	STANDARDASSETIMPORT_API auto RegisterStandardAssetImportProviders(
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto UnregisterStandardAssetImportProviders() -> void;
}
