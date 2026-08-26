#pragma once

#include "Terrain/TerrainWorldTile.h"

namespace Durin::AssetForge::Builtins
{
	struct FTerrainWorldBuildDiagnostics
	{
		Asset::ETerrainWorldOutcome Outcome =
			Asset::ETerrainWorldOutcome::Unavailable;
		uint64 NormalizeNanoseconds = 0;
		uint64 BuildNanoseconds = 0;
		uint64 PublishNanoseconds = 0;
		uint64 PeakTaskBytes = 0;
		uint64 ProductBytes = 0;
		uint32 LocalProductCount = 0;
		uint32 CachedProductCount = 0;
	};

	inline auto BuildAndPublishTerrainWorldTile(
		const Asset::FTerrainWorldDefinition& Definition,
		int64 TileX, int64 TileY,
		const Asset::FTerrainComposedTileValues& ComposedValues,
		const FGuid& GenerationId,
		Asset::FTerrainTileGenerationPublisher& Publisher,
		FTerrainWorldBuildDiagnostics& OutDiagnostics,
		std::string& OutError) -> bool
	{
		using namespace Asset;
		OutDiagnostics = {};
		const uint64 RequestId = Publisher.BeginRequest();
		const auto NormalizeStart = std::chrono::steady_clock::now();
		FTerrainNormalizedTileInput Input;
		if (!NormalizeTerrainTileInput(Definition, TileX, TileY, ComposedValues,
			Input, OutDiagnostics.Outcome, OutError)) return false;
		OutDiagnostics.NormalizeNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - NormalizeStart).count());
		OutDiagnostics.PeakTaskBytes = EstimateTerrainTileBuildBytes(Input);
		const auto BuildStart = std::chrono::steady_clock::now();
		FTerrainTileGeneration Candidate;
		if (!BuildTerrainTileGeneration(Input, GenerationId, Candidate,
			OutDiagnostics.Outcome, OutError)) return false;
		OutDiagnostics.BuildNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - BuildStart).count());
		for (const FTerrainTileProduct& Product : Candidate.Products)
		{
			OutDiagnostics.ProductBytes += Product.Bytes.size();
			if (Product.Origin == ETerrainTileBuildOrigin::DerivedData)
				++OutDiagnostics.CachedProductCount;
			else ++OutDiagnostics.LocalProductCount;
		}
		const auto PublishStart = std::chrono::steady_clock::now();
		if (!Publisher.Publish(RequestId, std::move(Candidate),
			OutDiagnostics.Outcome, OutError)) return false;
		OutDiagnostics.PublishNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - PublishStart).count());
		return true;
	}
}
