#pragma once

#include "StandardAssetImportAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin::Asset::Import::Standard
{
	// Registers the built-in Scene, geometry, and image importer descriptors.
	// Each descriptor owns its planning and reimport capabilities. Calls are idempotent.
	STANDARDASSETIMPORT_API auto RegisterStandardAssetImportProviders(
		std::string& OutError, FModuleOwnedCallbackGate OwnerGate) -> bool;
	STANDARDASSETIMPORT_API auto UnregisterStandardAssetImportProviders() -> void;
}
