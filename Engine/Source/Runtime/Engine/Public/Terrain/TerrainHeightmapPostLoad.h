#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DTerrainHeightmap;
	class ITerrainHeightmapAuthoringFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.TerrainHeightmapAuthoring";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto PostLoadUncooked(DTerrainHeightmap& Heightmap, std::string& OutError) -> bool = 0;
		virtual auto WaitForAuthoringLoad(DTerrainHeightmap& Heightmap, std::string& OutError) -> bool = 0;
		virtual auto ChangeSourceReference(
			DTerrainHeightmap& Heightmap,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeTerrainHeightmapUncookedPostLoadHandler(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool;
	ENGINE_API auto WaitForTerrainHeightmapAuthoringLoad(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool;
	ENGINE_API auto InvokeTerrainHeightmapSourceChangeHandler(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
}
