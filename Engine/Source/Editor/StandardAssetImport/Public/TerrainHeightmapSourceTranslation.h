#pragma once

#include "StandardAssetImportAPI.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	class DTerrainHeightmap;
}

namespace Durin::StandardAssetImport
{
	STANDARDASSETIMPORT_API auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings = {},
		bool bEngineAuthoringContext = false) -> FTerrainHeightmapImportResult;
	STANDARDASSETIMPORT_API auto ChangeTerrainHeightmapSourceReference(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto ReimportTerrainHeightmapSource(
		DTerrainHeightmap& Heightmap,
		std::string& OutError) -> bool;
}
