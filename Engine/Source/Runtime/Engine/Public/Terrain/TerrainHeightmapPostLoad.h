#pragma once

#include "EngineAPI.h"
#include "Modules/ModularFeature.h"

namespace Durin
{
	class DTerrainHeightmap;
	class ITerrainHeightmapDerivedDataLoadFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.TerrainHeightmapDerivedDataLoad";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto PostLoadUncooked(DTerrainHeightmap& Heightmap, std::string& OutError) -> bool = 0;
		virtual auto WaitForDerivedDataLoad(DTerrainHeightmap& Heightmap, std::string& OutError) -> bool = 0;
	};

	class ITerrainHeightmapSourceMutationFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Engine.TerrainHeightmapSourceMutation";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto ChangeSourceReference(
			DTerrainHeightmap& Heightmap,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool = 0;
	};

	ENGINE_API auto InvokeTerrainHeightmapUncookedPostLoadHandler(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool;
	ENGINE_API auto WaitForTerrainHeightmapDerivedDataLoad(
		DTerrainHeightmap& Heightmap, std::string& OutError) -> bool;
	ENGINE_API auto InvokeTerrainHeightmapSourceChangeHandler(
		DTerrainHeightmap& Heightmap,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
}
