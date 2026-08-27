#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Serialization/PayloadDecodeResult.h"

namespace Durin
{
	struct FTerrainHeightmapPayload;

	inline constexpr uint32 TerrainHeightmapPayloadSchemaVersion = 2;
	inline constexpr uint32 TerrainHeightmapBuilderVersion = 3;
	inline constexpr uint32 TerrainHeightmapKeySchemaVersion = 3;
	inline constexpr uint32 TerrainHeightmapBaseRegionSize = 64;
	inline constexpr uint32 TerrainHeightmapPayloadHeaderSize = 96;
	inline constexpr uint32 TerrainHeightmapLevelRecordSize = 24;
	inline constexpr uint32 TerrainHeightmapPayloadAlignment = 16;
	inline constexpr uint32 MaximumTerrainHeightmapDimension = 16'384;
	inline constexpr uint64 MaximumTerrainHeightmapSamples = 268'435'456;
	inline constexpr uint64 MaximumTerrainHeightmapEncodedBytes = 512ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumTerrainHeightmapPayloadBytes = 513ull * 1024ull * 1024ull;
	inline constexpr uint64 MaximumTerrainHeightmapHierarchyBytes = 512ull * 1024ull;
	inline constexpr uint64 MaximumTerrainHeightmapPeakBuildBytes = 2'560ull * 1024ull * 1024ull;
	inline const FGuid TerrainHeightmapPrimaryCookedPayloadId{
		0x7d0d1524, 0x69ba42a9, 0x91f70da3, 0x47bc2861};

}
