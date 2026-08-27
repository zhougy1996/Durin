#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/PackageSerialization.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin
{
	class DTerrainHeightmap;
}

namespace Durin::AssetForge::Builtins
{
	ASSETFORGEBUILTINS_API auto IsTerrainHeightmapSourceExtension(
		std::string_view Extension) -> bool;
	// Carries one admitted terrain source into the source-format-neutral canonical builder.
	struct FTerrainHeightmapSourceData
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;

		auto IsValid() const -> bool
		{
			return Width != 0 && Height != 0
				&& Samples.size() == static_cast<size_t>(Width) * Height;
		}
	};

	ASSETFORGEBUILTINS_API auto TranslateTerrainHeightmapSource(
		std::string_view Extension,
		std::span<const std::byte> EncodedBytes,
		FTerrainHeightmapSourceData& OutSource,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto ImportTerrainHeightmapAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FTerrainHeightmapImportSettings& Settings = {},
		bool bAllowEngineContentWrite = false) -> FTerrainHeightmapImportResult;
	ASSETFORGEBUILTINS_API auto ReimportTerrainHeightmapSource(
		DTerrainHeightmap& Heightmap,
		std::string& OutError,
		const Asset::FAssetBundleSaveOptions& SaveOptions = {}) -> bool;
}
