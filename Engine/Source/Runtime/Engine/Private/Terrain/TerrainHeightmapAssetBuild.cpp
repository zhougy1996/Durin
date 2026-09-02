#include "Terrain/TerrainHeightmapAssetBuild.h"

#include "Terrain/TerrainHeightmapBuild.h"

namespace Durin
{
	auto PrepareTerrainHeightmapPayload(DTerrainHeightmap& Heightmap, std::string& OutError) -> bool
	{
		FTerrainHeightmapDerivedDataResult Result;
		if (!BuildTerrainHeightmapDerivedData({
			.ImportedData = Heightmap.GetImportedData()}, Result, OutError)) return false;
		return Heightmap.SetPayload(std::move(Result.Payload), OutError,
			false, std::move(Result.ImportedData));
	}
}
