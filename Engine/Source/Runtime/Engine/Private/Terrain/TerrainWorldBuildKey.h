#pragma once

#if DURIN_WITH_EDITOR
#include "Terrain/TerrainWorld.h"

namespace Durin
{
	ENGINE_API auto MakeTerrainTileBuildKey(
		const FTerrainTileRecipeInput& Input, ETerrainTileProductClass ProductClass,
		std::string& OutError) -> std::string;
}
#endif
