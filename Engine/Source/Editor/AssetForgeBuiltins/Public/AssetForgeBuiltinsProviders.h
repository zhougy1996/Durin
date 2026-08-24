#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin::AssetForge::Builtins
{
	// Registers the built-in Scene, geometry, and image importer descriptors.
	// Each descriptor owns its planning and reimport capabilities. Calls are idempotent.
	ASSETFORGEBUILTINS_API auto RegisterAssetForgeBuiltinsProviders(
		std::string& OutError, FModuleOwnedCallbackGate OwnerGate) -> bool;
	ASSETFORGEBUILTINS_API auto UnregisterAssetForgeBuiltinsProviders() -> void;
}
