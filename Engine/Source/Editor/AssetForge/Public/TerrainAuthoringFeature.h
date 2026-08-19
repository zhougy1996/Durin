#pragma once

#include "Modules/AsyncOperationGroup.h"
#include "AssetForgeAPI.h"
#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin::Asset::Forge
{
	struct FTerrainAuthoringState;

	class FTerrainAuthoringFeature final : public ITerrainHeightmapAuthoringFeature
	{
	public:
		ASSETFORGE_API FTerrainAuthoringFeature();
		ASSETFORGE_API ~FTerrainAuthoringFeature() override;
		FTerrainAuthoringFeature(const FTerrainAuthoringFeature&) = delete;
		auto operator=(const FTerrainAuthoringFeature&) -> FTerrainAuthoringFeature& = delete;

		ASSETFORGE_API auto SetOperationGroup(FAsyncOperationGroup Group) -> bool;
		ASSETFORGE_API auto Shutdown() -> void;
		ASSETFORGE_API auto PostLoadUncooked(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool override;
		ASSETFORGE_API auto WaitForAuthoringLoad(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool override;
		ASSETFORGE_API auto ChangeSourceReference(
			DTerrainHeightmap& Heightmap,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool override;

	private:
		std::unique_ptr<FTerrainAuthoringState> State;
	};
}
