#pragma once

#include "EngineAssetBuildAPI.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin::AssetBuild
{
	ENGINEASSETBUILD_API auto BuildTerrainHeightmapFromEncodedBytes(
		DTerrainHeightmap& Heightmap,
		std::span<const uint8> EncodedBytes,
		const FSourcePath& SourcePath,
		std::string& OutError) -> bool;

	ENGINEASSETBUILD_API auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings = {},
		bool bEngineAuthoringContext = false) -> FTerrainHeightmapImportResult;
}
