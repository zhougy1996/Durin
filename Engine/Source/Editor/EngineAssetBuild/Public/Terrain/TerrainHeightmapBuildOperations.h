#pragma once

#include "EngineAssetBuildAPI.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin::AssetBuild
{
	struct FTerrainHeightmapBuildRequest
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
		bool bPersistDerivedData = true;
	};

	struct FTerrainHeightmapBuildProduct
	{
		std::shared_ptr<const FTerrainHeightmapPayload> Payload;
		std::string DerivedDataKey;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
	};

	struct FTerrainHeightmapPublicationContext
	{
		FSourcePath SourcePath;
		std::string DecoderId = "DurinImage";
		uint32 DecoderVersion = 1;
		uint64 SourceFileSize = 0;
		int64 SourceLastWriteTime = 0;
	};

	ENGINEASSETBUILD_API auto BuildTerrainHeightmap(
		FTerrainHeightmapBuildRequest Request,
		FTerrainHeightmapBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	ENGINEASSETBUILD_API auto PublishTerrainHeightmapProduct(
		DTerrainHeightmap& Heightmap,
		FTerrainHeightmapBuildProduct Product,
		const FTerrainHeightmapPublicationContext& Context,
		std::string& OutError) -> bool;

	ENGINEASSETBUILD_API auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings = {},
		bool bEngineAuthoringContext = false) -> FTerrainHeightmapImportResult;
}
