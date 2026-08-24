#pragma once

#include "AssetForge/Extensions/ComponentRegistration.h"

namespace Durin::AssetForge
{
	class FImportService;
}

namespace Durin::AssetForge::Builtins
{
	auto RegisterImageFamilyImports(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& OutRegistrations,
		std::string& OutError) -> bool;
	auto RegisterTextureCubeImports(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& OutRegistrations,
		std::string& OutError) -> bool;
	auto RegisterVolumeTextureImports(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& OutRegistrations,
		std::string& OutError) -> bool;
	auto RegisterTerrainHeightmapImports(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& OutRegistrations,
		std::string& OutError) -> bool;
}
