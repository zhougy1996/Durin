#pragma once

#include "AssetForgeAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin::Asset::Forge
{
	// Registers the built-in Scene, geometry, and image importer descriptors.
	// Each descriptor owns its planning and reimport capabilities. Calls are idempotent.
	ASSETFORGE_API auto RegisterAssetForgeProviders(
		std::string& OutError, FModuleOwnedCallbackGate OwnerGate) -> bool;
	ASSETFORGE_API auto UnregisterAssetForgeProviders() -> void;
}
