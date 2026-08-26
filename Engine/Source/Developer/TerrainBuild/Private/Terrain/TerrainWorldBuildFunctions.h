#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"
#include "Terrain/TerrainWorldTile.h"

namespace Durin::Asset::Private
{
	using namespace ::Durin::DerivedData;

	inline constexpr std::string_view TerrainWorldProductInputName = "TerrainWorldProductBody";

	auto GetTerrainWorldBuildFunctionIdentity(ETerrainTileProductClass ProductClass)
		-> FBuildFunctionIdentity;
	auto GetTerrainWorldBuildValueName(ETerrainTileProductClass ProductClass)
		-> std::string_view;
	auto CreateTerrainWorldBuildFunction(ETerrainTileProductClass ProductClass)
		-> std::shared_ptr<IBuildFunction>;
	auto ValidateTerrainWorldProductBody(ETerrainTileProductClass ProductClass,
		std::span<const std::byte> Bytes, std::string& OutError) -> bool;
}
