#pragma once

#include "GeometryBuildAPI.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin::Asset::Build
{
	struct FTerrainHeightmapBuildRequest
	{
		std::vector<uint16> Samples;
		uint32 Width = 0;
		uint32 Height = 0;
		uint64 SourceContentHashLow = 0;
		uint64 SourceContentHashHigh = 0;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;
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
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;
		uint64 SourceFileSize = 0;
		int64 SourceLastWriteTime = 0;
		bool bAdvanceRevision = true;
		bool bMarkPackageDirty = true;
	};

	GEOMETRYBUILD_API auto BuildTerrainHeightmap(
		FTerrainHeightmapBuildRequest Request,
		FTerrainHeightmapBuildProduct& OutProduct,
		std::string& OutError) -> bool;
	GEOMETRYBUILD_API auto PublishTerrainHeightmapProduct(
		DTerrainHeightmap& Heightmap,
		FTerrainHeightmapBuildProduct Product,
		const FTerrainHeightmapPublicationContext& Context,
		std::string& OutError) -> bool;
	GEOMETRYBUILD_API auto MakeTerrainHeightmapDerivedDataKey(
		const DTerrainHeightmap& Heightmap,
		std::string& OutError) -> std::string;
	GEOMETRYBUILD_API auto LoadTerrainHeightmapDerivedData(
		std::string_view Key,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError) -> bool;

}
