#pragma once

#include "GeometryBuildAPI.h"
#include "Hash/XxHash.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin::Asset::Build
{
	struct FTerrainHeightmapBuildKeyInput
	{
		FXxHash128 SourceContentHash;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat = ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;
		uint32 BuilderVersion = TerrainHeightmapBuilderVersion;
		uint32 PayloadSchemaVersion = TerrainHeightmapPayloadSchemaVersion;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Invalid;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Invalid;

		GEOMETRYBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	GEOMETRYBUILD_API auto BuildTerrainHeightmapDerivedDataKeyBytes(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::vector<uint8>;
	GEOMETRYBUILD_API auto BuildTerrainHeightmapDerivedDataKey(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::string;
}
