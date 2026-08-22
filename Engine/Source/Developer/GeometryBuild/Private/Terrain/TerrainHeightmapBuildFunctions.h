#pragma once

#include "AssetBuild/BuildFunction.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"

namespace Durin::Asset::Build::Private
{
	extern const FBuildFunctionIdentity TerrainHeightmapFunctionIdentity;
	inline constexpr std::string_view TerrainHeightmapInputName =
		"TerrainHeightmapBuildInput";
	inline constexpr std::string_view TerrainHeightmapValueName =
		"TerrainHeightmapPayload";

	auto EncodeTerrainHeightmapLocalInput(const FTerrainHeightmapBuildRequest& Request)
		-> std::vector<std::byte>;
	auto DecodeTerrainHeightmapPayload(const FBuildValue& Value,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError) -> bool;
	auto CreateTerrainHeightmapBuildFunction() -> std::shared_ptr<IBuildFunction>;
}
