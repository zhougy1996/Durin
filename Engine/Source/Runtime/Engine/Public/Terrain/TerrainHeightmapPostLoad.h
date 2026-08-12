#pragma once

#include "EngineAPI.h"

namespace Durin
{
	class DTerrainHeightmap;
	using FTerrainHeightmapUncookedPostLoadHandler =
		std::function<bool(DTerrainHeightmap&, std::string&)>;
	using FTerrainHeightmapSourceChangeHandler =
		std::function<bool(DTerrainHeightmap&, std::string_view, std::string&)>;

	ENGINE_API auto RegisterTerrainHeightmapUncookedPostLoadHandler(
		FTerrainHeightmapUncookedPostLoadHandler Handler) -> bool;
	ENGINE_API auto UnregisterTerrainHeightmapUncookedPostLoadHandler() -> void;
	ENGINE_API auto InvokeTerrainHeightmapUncookedPostLoadHandler(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool;
	ENGINE_API auto RegisterTerrainHeightmapSourceChangeHandler(
		FTerrainHeightmapSourceChangeHandler Handler) -> bool;
	ENGINE_API auto UnregisterTerrainHeightmapSourceChangeHandler() -> void;
	ENGINE_API auto InvokeTerrainHeightmapSourceChangeHandler(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
}
