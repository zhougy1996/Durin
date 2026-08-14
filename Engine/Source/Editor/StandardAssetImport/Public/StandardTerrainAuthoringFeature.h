#pragma once

#include "Modules/AsyncOperationGroup.h"
#include "StandardAssetImportAPI.h"
#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin::Asset::Import::Standard
{
	struct FStandardTerrainAuthoringState;

	class FStandardTerrainAuthoringFeature final : public ITerrainHeightmapAuthoringFeature
	{
	public:
		STANDARDASSETIMPORT_API FStandardTerrainAuthoringFeature();
		STANDARDASSETIMPORT_API ~FStandardTerrainAuthoringFeature() override;
		FStandardTerrainAuthoringFeature(const FStandardTerrainAuthoringFeature&) = delete;
		auto operator=(const FStandardTerrainAuthoringFeature&) -> FStandardTerrainAuthoringFeature& = delete;

		STANDARDASSETIMPORT_API auto SetOperationGroup(FAsyncOperationGroup Group) -> bool;
		STANDARDASSETIMPORT_API auto Shutdown() -> void;
		STANDARDASSETIMPORT_API auto PostLoadUncooked(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool override;
		STANDARDASSETIMPORT_API auto WaitForAuthoringLoad(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool override;
		STANDARDASSETIMPORT_API auto ChangeSourceReference(
			DTerrainHeightmap& Heightmap,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool override;

	private:
		std::unique_ptr<FStandardTerrainAuthoringState> State;
	};
}
