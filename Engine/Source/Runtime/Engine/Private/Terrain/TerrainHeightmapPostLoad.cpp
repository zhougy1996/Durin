#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin
{
	namespace
	{
		std::mutex GMutex;
		FTerrainHeightmapUncookedPostLoadHandler GHandler;
		FTerrainHeightmapSourceChangeHandler GSourceChangeHandler;
		FTerrainHeightmapAuthoringLoadWaitHandler GWaitHandler;
	}

	auto RegisterTerrainHeightmapAuthoringLoadWaitHandler(
		FTerrainHeightmapAuthoringLoadWaitHandler Handler) -> bool
	{
		if (!Handler) return false;
		std::lock_guard Lock(GMutex);
		if (GWaitHandler) return false;
		GWaitHandler = std::move(Handler);
		return true;
	}

	auto UnregisterTerrainHeightmapAuthoringLoadWaitHandler() -> void
	{
		std::lock_guard Lock(GMutex);
		GWaitHandler = {};
	}

	auto WaitForTerrainHeightmapAuthoringLoad(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		FTerrainHeightmapAuthoringLoadWaitHandler Handler;
		{
			std::lock_guard Lock(GMutex);
			Handler = GWaitHandler;
		}
		if (!Handler)
		{
			OutError = "No TerrainHeightmap authoring-load wait policy is registered.";
			return false;
		}
		return Handler(Heightmap, OutError);
	}

	auto RegisterTerrainHeightmapUncookedPostLoadHandler(
		FTerrainHeightmapUncookedPostLoadHandler Handler) -> bool
	{
		if (!Handler) return false;
		std::lock_guard Lock(GMutex);
		if (GHandler) return false;
		GHandler = std::move(Handler);
		return true;
	}

	auto UnregisterTerrainHeightmapUncookedPostLoadHandler() -> void
	{
		std::lock_guard Lock(GMutex);
		GHandler = {};
	}

	auto InvokeTerrainHeightmapUncookedPostLoadHandler(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		FTerrainHeightmapUncookedPostLoadHandler Handler;
		{
			std::lock_guard Lock(GMutex);
			Handler = GHandler;
		}
		if (!Handler)
		{
			OutError = "No uncooked TerrainHeightmap load policy is registered.";
			return false;
		}
		return Handler(Heightmap, OutError);
	}

	auto RegisterTerrainHeightmapSourceChangeHandler(
		FTerrainHeightmapSourceChangeHandler Handler) -> bool
	{
		if (!Handler) return false;
		std::lock_guard Lock(GMutex);
		if (GSourceChangeHandler) return false;
		GSourceChangeHandler = std::move(Handler);
		return true;
	}

	auto UnregisterTerrainHeightmapSourceChangeHandler() -> void
	{
		std::lock_guard Lock(GMutex);
		GSourceChangeHandler = {};
	}

	auto InvokeTerrainHeightmapSourceChangeHandler(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool
	{
		FTerrainHeightmapSourceChangeHandler Handler;
		{
			std::lock_guard Lock(GMutex);
			Handler = GSourceChangeHandler;
		}
		if (!Handler)
		{
			OutError = "No TerrainHeightmap source-change policy is registered.";
			return false;
		}
		return Handler(Heightmap, SourceVirtualPath, OutError);
	}
}
