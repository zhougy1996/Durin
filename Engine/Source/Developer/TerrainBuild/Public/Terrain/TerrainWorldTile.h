#pragma once

#include "TerrainBuildAPI.h"
#include "Terrain/TerrainWorldBuildProvider.h"

namespace Durin
{
	TERRAINBUILD_API auto NormalizeTerrainTileInput(
		const FTerrainWorldDefinition& Definition, int64 TileX, int64 TileY,
		const FTerrainComposedTileValues& ComposedValues,
		FTerrainTileRecipeInput& OutInput,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	TERRAINBUILD_API auto ComposeTerrainTileInput(
		const FTerrainWorldDefinition& Definition, int64 TileX, int64 TileY,
		std::span<const FTerrainTileSourceContribution> Contributions,
		FTerrainTileRecipeInput& OutInput,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError,
		std::function<bool()> ShouldCancel = {}) -> bool;
	TERRAINBUILD_API auto EstimateTerrainTileBuildBytes(
		const FTerrainTileRecipeInput& Input) -> uint64;
	TERRAINBUILD_API auto BuildTerrainNeighborEvidence(
		const FTerrainTileRecipeInput& Tile,
		const FTerrainTileRecipeInput& Neighbor,
		FTerrainNeighborEvidence& OutEvidence,
		ETerrainWorldOutcome& OutOutcome, std::string& OutError) -> bool;
	TERRAINBUILD_API auto BuildTerrainWorldRecipe(
		FTerrainWorldRecipeRequest Request,
		FTerrainWorldRecipeProduct& OutProduct,
		ETerrainWorldOutcome& OutOutcome,
		std::string& OutError) -> bool;
}
