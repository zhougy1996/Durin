#pragma once

#if DURIN_WITH_EDITOR

#include "Asset/CookedAsset.h"
#include "EngineAPI.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapDerivedData.h"

namespace Durin
{
	inline constexpr uint32 TerrainHeightmapKeySchemaVersion = 3;

	// Canonical Engine-owned identity for one TerrainHeightmap derived value.
	struct FTerrainHeightmapBuildKeyInput
	{
		FXxHash128 SourceContentHash;
		std::string DecoderId;
		uint32 DecoderVersion = 0;
		ETerrainHeightmapSourceFormat SourceFormat =
			ETerrainHeightmapSourceFormat::Unknown;
		uint32 SourceProfileVersion = 0;
		uint32 BuilderVersion = TerrainHeightmapBuilderVersion;
		uint32 PayloadSchemaVersion = TerrainHeightmapPayloadSchemaVersion;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;

		ENGINE_API auto Serialize(FArchive& Ar) -> void;
	};

	ENGINE_API auto BuildTerrainHeightmapDerivedDataKeyBytes(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> FByteArray;
	ENGINE_API auto BuildTerrainHeightmapDerivedDataKey(
		const FTerrainHeightmapBuildKeyInput& Input,
		std::string& OutError) -> std::string;
}

#endif
