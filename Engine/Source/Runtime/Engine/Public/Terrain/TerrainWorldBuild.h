#pragma once

#include "Terrain/TerrainWorldBuildProvider.h"

namespace Durin
{
	enum class ETerrainTileBuildOrigin : uint8
	{
		LocalBuild,
		DerivedData
	};


	struct FTerrainWorldDerivedDataRequest
	{
		FTerrainTileRecipeInput Input;
		FGuid GenerationId;
		bool bQueryDerivedData = true;
		bool bPersistDerivedData = true;
	};

	struct FTerrainWorldDerivedDataResult
	{
		FTerrainTileGeneration Generation;
		FTerrainWorldBuildProviderDescriptor Descriptor;
		std::array<std::string, 5> Keys;
		std::array<ETerrainTileBuildOrigin, 5> Origins{};
		std::array<uint64, 5> PayloadBytes{};
		std::array<uint64, 5> CacheReadNanoseconds{};
		std::array<uint64, 5> CacheWriteNanoseconds{};
		std::array<std::string, 5> Diagnostics;
	};

	ENGINE_API auto BuildTerrainWorldDerivedData(
		FTerrainWorldDerivedDataRequest Request,
		FTerrainWorldDerivedDataResult& OutResult,
		ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool;
}
