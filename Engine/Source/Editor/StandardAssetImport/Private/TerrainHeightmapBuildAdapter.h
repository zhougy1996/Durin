#pragma once

#include "EncodedSourceSnapshot.h"
#include "TerrainHeightmapSourceTranslation.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"

namespace Durin::Asset::Import::Standard
{
	inline auto BuildTerrainHeightmapFromSource(
		DTerrainHeightmap& Heightmap,
		FTerrainHeightmapSourceData SourceData,
		const FEncodedSourceSnapshot& Source,
		std::string& OutError,
		bool bAdvanceRevision = true,
		bool bMarkPackageDirty = true) -> bool
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
			.SourceProfileVersion = SourceData.SourceProfileVersion}, Product, OutError)) return false;
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
