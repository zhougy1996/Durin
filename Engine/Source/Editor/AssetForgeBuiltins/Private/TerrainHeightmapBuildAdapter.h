#pragma once

#include "EncodedSourceSnapshot.h"
#include "AssetForge/Builtins/TerrainHeightmapImport.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	inline auto BuildTerrainHeightmapFromSource(
		DTerrainHeightmap& Heightmap,
		FTerrainHeightmapSourceData SourceData,
		const FEncodedSourceSnapshot& Source,
		std::string& OutError,
		bool bAdvanceRevision = true,
		bool bMarkPackageDirty = true,
		bool bQueryDerivedData = true) -> bool
	{
		Asset::Build::FTerrainHeightmapBuildProduct Product;
		if (!Asset::Build::BuildTerrainHeightmap({
			.Samples = std::move(SourceData.Samples),
			.Width = SourceData.Width,
			.Height = SourceData.Height,
			.SourceContentHashLow = Source.ContentHash.HashLow,
			.SourceContentHashHigh = Source.ContentHash.HashHigh,
			.DecoderId = SourceData.DecoderId,
			.DecoderVersion = SourceData.DecoderVersion,
			.SourceFormat = SourceData.SourceFormat,
			.SourceProfileVersion = SourceData.SourceProfileVersion,
			.bQueryDerivedData = bQueryDerivedData}, Product, OutError)) return false;
		return Asset::Build::PublishTerrainHeightmapProduct(Heightmap, std::move(Product), {
			.SourcePath = Source.SourcePath,
			.DecoderId = SourceData.DecoderId,
			.DecoderVersion = SourceData.DecoderVersion,
			.SourceFormat = SourceData.SourceFormat,
			.SourceProfileVersion = SourceData.SourceProfileVersion,
			.SourceFileSize = Source.FileSize,
			.bAdvanceRevision = bAdvanceRevision,
			.bMarkPackageDirty = bMarkPackageDirty}, OutError);
	}
}
