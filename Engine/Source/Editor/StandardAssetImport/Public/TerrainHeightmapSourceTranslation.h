#pragma once

#include "StandardAssetImportAPI.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	class DTerrainHeightmap;
}

namespace Durin::Asset::Import
{
	// Carries one admitted Terrain source into the source-format-neutral canonical builder.
	struct FTerrainHeightmapDecodedSource
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;
	};

	STANDARDASSETIMPORT_API auto DecodeTerrainHeightmapSource(
		std::string_view Extension,
		std::span<const uint8> Bytes,
		FTerrainHeightmapDecodedSource& OutSource,
		std::string& OutError) -> bool;
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
