#pragma once

#include "Modules/AsyncOperationGroup.h"
#include "AssetForgeBuiltinsAPI.h"
#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin::AssetForge::Builtins
{
	struct FTerrainAuthoringState;

	class FTerrainAuthoringFeature final : public ITerrainHeightmapAuthoringFeature
	{
	public:
		ASSETFORGEBUILTINS_API FTerrainAuthoringFeature();
		ASSETFORGEBUILTINS_API ~FTerrainAuthoringFeature() override;
		FTerrainAuthoringFeature(const FTerrainAuthoringFeature&) = delete;
		auto operator=(const FTerrainAuthoringFeature&) -> FTerrainAuthoringFeature& = delete;

		ASSETFORGEBUILTINS_API auto SetOperationGroup(FAsyncOperationGroup Group) -> bool;
		ASSETFORGEBUILTINS_API auto Shutdown() -> void;
		ASSETFORGEBUILTINS_API auto PostLoadUncooked(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto WaitForAuthoringLoad(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto ChangeSourceReference(
			DTerrainHeightmap& Heightmap,
			std::string_view SourceVirtualPath,
			std::string& OutError) -> bool override;

	private:
		std::unique_ptr<FTerrainAuthoringState> State;
	};
}
