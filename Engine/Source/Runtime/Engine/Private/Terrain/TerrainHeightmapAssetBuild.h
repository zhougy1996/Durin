#pragma once

namespace Durin
{
	class DTerrainHeightmap;
	auto PrepareTerrainHeightmapPayload(DTerrainHeightmap& Heightmap, std::string& OutError) -> bool;
}
