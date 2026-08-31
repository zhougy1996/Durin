#pragma once

#include "DerivedDataCache/DerivedDataBuildFunction.h"
#include "Terrain/TerrainHeightmapBuildOperations.h"

namespace Durin::Asset::Private
{
	using namespace ::Durin::DerivedData;

	extern const FBuildFunctionName TerrainHeightmapFunctionName;
	inline constexpr std::string_view TerrainHeightmapInputName =
		"TerrainHeightmapBuildInput";
	inline constexpr std::string_view TerrainHeightmapValueName =
		"TerrainHeightmapPayload";

	auto EncodeTerrainHeightmapLocalInput(const FTerrainHeightmapBuildRequest& Request)
		-> FByteArray;
	auto DecodeTerrainHeightmapPayload(const FBuildValue& Value,
		std::shared_ptr<const FTerrainHeightmapPayload>& OutPayload,
		std::string& OutError) -> bool;
	auto CreateTerrainHeightmapBuildFunction() -> std::shared_ptr<IBuildFunction>;
}
