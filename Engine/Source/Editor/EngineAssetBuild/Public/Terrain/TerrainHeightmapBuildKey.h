#pragma once

#include "EngineAssetBuildAPI.h"
#include "Hash/XxHash.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin::AssetBuild
{
	struct FTerrainHeightmapBuildKeyInput
	{
		FXxHash128 SourceContentHash;
		uint32 BuilderVersion = TerrainHeightmapBuilderVersion;
		uint32 PayloadSchemaVersion = TerrainHeightmapPayloadSchemaVersion;
		Asset::ECookTargetPlatform TargetPlatform = Asset::ECookTargetPlatform::Invalid;
		Asset::ECookTargetProfile TargetProfile = Asset::ECookTargetProfile::Invalid;

		ENGINEASSETBUILD_API auto Serialize(FArchive& Ar) -> void;
	};

	ENGINEASSETBUILD_API auto BuildTerrainHeightmapDerivedDataKeyBytes(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::vector<uint8>;
	ENGINEASSETBUILD_API auto BuildTerrainHeightmapDerivedDataKey(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::string;
}
