#pragma once

#include "DObject/ObjectHandle.h"

namespace Durin
{
	class DTerrainHeightmap;

	// Replaces registered Terrain render and physics state around one atomic heightmap publication.
	class FTerrainHeightmapRenderStateRecreateContext final
	{
	public:
		explicit FTerrainHeightmapRenderStateRecreateContext(DTerrainHeightmap* Heightmap);
		~FTerrainHeightmapRenderStateRecreateContext();
		FTerrainHeightmapRenderStateRecreateContext(const FTerrainHeightmapRenderStateRecreateContext&) = delete;
		auto operator=(const FTerrainHeightmapRenderStateRecreateContext&) -> FTerrainHeightmapRenderStateRecreateContext& = delete;

	private:
		FObjectHandle HeightmapHandle;
		std::vector<FObjectHandle> ComponentHandles;
	};
} // namespace Durin
