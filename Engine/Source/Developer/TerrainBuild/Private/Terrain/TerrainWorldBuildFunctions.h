#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"
#include "Terrain/TerrainWorldTile.h"

namespace Durin::AssetPrivate
{
	using namespace ::Durin::DerivedData;

	inline constexpr std::string_view TerrainWorldProductInputName = "TerrainWorldProductBody";

	auto GetTerrainWorldBuildFunctionName(ETerrainTileProductClass ProductClass)
		-> FBuildFunctionName;
	auto GetTerrainWorldBuildValueName(ETerrainTileProductClass ProductClass)
		-> std::string_view;
	auto CreateTerrainWorldBuildFunction(ETerrainTileProductClass ProductClass)
		-> std::shared_ptr<IBuildFunction>;
	auto ValidateTerrainWorldProductBody(ETerrainTileProductClass ProductClass,
		std::span<const std::byte> Bytes, std::string& OutError) -> bool;
}
