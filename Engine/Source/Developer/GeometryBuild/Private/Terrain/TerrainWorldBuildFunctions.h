#pragma once

#include "AssetBuild/BuildFunction.h"
#include "Terrain/TerrainWorldTile.h"

namespace Durin::Asset::Build::Private
{
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
