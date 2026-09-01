#pragma once

#include "TerrainBuildAPI.h"
#include "Hash/XxHash.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin
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
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;

		TERRAINBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	TERRAINBUILD_API auto BuildTerrainHeightmapDerivedDataKeyBytes(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> FByteArray;
	TERRAINBUILD_API auto BuildTerrainHeightmapDerivedDataKey(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::string;
}
