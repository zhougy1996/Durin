#pragma once

#include "Terrain/TerrainWorldTile.h"
#include "Terrain/TerrainWorldBuild.h"

namespace Durin::AssetForge::Builtins
{
	struct FTerrainWorldBuildDiagnostics
	{
		ETerrainWorldOutcome Outcome =
			ETerrainWorldOutcome::Unavailable;
		uint64 NormalizeNanoseconds = 0;
		uint64 BuildNanoseconds = 0;
		uint64 PublishNanoseconds = 0;
		uint64 PeakTaskBytes = 0;
		uint64 ProductBytes = 0;
		uint32 LocalProductCount = 0;
		uint32 CachedProductCount = 0;
	};

	inline auto BuildAndPublishTerrainWorldTile(
		const FTerrainWorldDefinition& Definition,
		int64 TileX, int64 TileY,
		const FTerrainComposedTileValues& ComposedValues,
		const FGuid& GenerationId,
		FTerrainTileGenerationPublisher& Publisher,
		FTerrainWorldBuildDiagnostics& OutDiagnostics,
		std::string& OutError) -> bool
	{
		using namespace Durin;
		OutDiagnostics = {};
		const uint64 RequestId = Publisher.BeginRequest();
		const auto NormalizeStart = std::chrono::steady_clock::now();
		FTerrainTileRecipeInput Input;
		if (!NormalizeTerrainTileInput(Definition, TileX, TileY, ComposedValues,
			Input, OutDiagnostics.Outcome, OutError)) return false;
		OutDiagnostics.NormalizeNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - NormalizeStart).count());
		OutDiagnostics.PeakTaskBytes = EstimateTerrainTileBuildBytes(Input);
		const auto BuildStart = std::chrono::steady_clock::now();
		FTerrainWorldDerivedDataResult Result;
		if (!BuildTerrainWorldDerivedData({std::move(Input), GenerationId}, Result,
			OutDiagnostics.Outcome, OutError)) return false;
		OutDiagnostics.BuildNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - BuildStart).count());
		for (size_t Index = 0; Index < Result.Generation.Products.size(); ++Index)
		{
			OutDiagnostics.ProductBytes += Result.Generation.Products[Index].Bytes.size();
			if (Result.Origins[Index] == ETerrainTileBuildOrigin::DerivedData)
				++OutDiagnostics.CachedProductCount;
			else ++OutDiagnostics.LocalProductCount;
		}
		const auto PublishStart = std::chrono::steady_clock::now();
		if (!Publisher.Publish(RequestId, std::move(Result.Generation),
			OutDiagnostics.Outcome, OutError)) return false;
		OutDiagnostics.PublishNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - PublishStart).count());
		return true;
	}
}
