#include "ImageFamilyImports.h"

namespace Durin::AssetForge::Builtins
{
	auto RegisterImageFamilyImports(
		FImportService& Service,
		FModuleOwnedCallbackGate OwnerGate,
		std::vector<FComponentRegistration>& OutRegistrations,
		std::string& OutError) -> bool
	{
		return RegisterTextureCubeImports(
				Service, OwnerGate, OutRegistrations, OutError)
			&& RegisterVolumeTextureImports(
				Service, OwnerGate, OutRegistrations, OutError)
			&& RegisterTerrainHeightmapImports(
				Service, std::move(OwnerGate), OutRegistrations, OutError);
	}
}
