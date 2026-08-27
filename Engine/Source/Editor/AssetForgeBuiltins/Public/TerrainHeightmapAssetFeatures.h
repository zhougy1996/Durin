#pragma once

#include "Modules/AsyncOperationGroup.h"
#include "AssetForgeBuiltinsAPI.h"
#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin::AssetForge::Builtins
{
	struct FTerrainHeightmapDerivedDataLoadState;

	class FTerrainHeightmapAssetFeatures final
		: public ITerrainHeightmapDerivedDataLoadFeature
	{
	public:
		ASSETFORGEBUILTINS_API FTerrainHeightmapAssetFeatures();
		ASSETFORGEBUILTINS_API ~FTerrainHeightmapAssetFeatures() override;
		FTerrainHeightmapAssetFeatures(const FTerrainHeightmapAssetFeatures&) = delete;
		auto operator=(const FTerrainHeightmapAssetFeatures&) -> FTerrainHeightmapAssetFeatures& = delete;

		ASSETFORGEBUILTINS_API auto SetOperationGroup(FAsyncOperationGroup Group) -> bool;
		ASSETFORGEBUILTINS_API auto Shutdown() -> void;
		ASSETFORGEBUILTINS_API auto PostLoadUncooked(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool override;
		ASSETFORGEBUILTINS_API auto WaitForDerivedDataLoad(
			DTerrainHeightmap& Heightmap, std::string& OutError) -> bool override;
	private:
		std::unique_ptr<FTerrainHeightmapDerivedDataLoadState> State;
	};
}
