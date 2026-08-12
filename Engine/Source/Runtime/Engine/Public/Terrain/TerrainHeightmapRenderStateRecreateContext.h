#pragma once

#include "DObject/ObjectHandle.h"
#include "EngineAPI.h"

namespace Durin
{
	class DTerrainHeightmap;

	// Replaces registered Terrain proxies around one atomic heightmap publication.
	class FTerrainHeightmapRenderStateRecreateContext final
	{
	public:
		ENGINE_API explicit FTerrainHeightmapRenderStateRecreateContext(DTerrainHeightmap* Heightmap);
		ENGINE_API ~FTerrainHeightmapRenderStateRecreateContext();
		FTerrainHeightmapRenderStateRecreateContext(const FTerrainHeightmapRenderStateRecreateContext&) = delete;
		auto operator=(const FTerrainHeightmapRenderStateRecreateContext&) -> FTerrainHeightmapRenderStateRecreateContext& = delete;

	private:
		FObjectHandle HeightmapHandle;
		std::vector<FObjectHandle> ComponentHandles;
	};
} // namespace Durin
