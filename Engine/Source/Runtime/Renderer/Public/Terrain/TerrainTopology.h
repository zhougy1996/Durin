#pragma once

#include "RendererAPI.h"

#include <functional>

namespace Durin
{
	enum class ETerrainStitchEdge : uint8
	{
		North = 1u << 0,
		East = 1u << 1,
		South = 1u << 2,
		West = 1u << 3,
	};

	struct FTerrainTopologyKey
	{
		uint16 CellCountX = 0;
		uint16 CellCountY = 0;
		uint16 LODStep = 1;
		uint8 StitchMask = 0;

		auto operator==(const FTerrainTopologyKey&) const -> bool = default;
	};

	struct FTerrainTopologyData
	{
		std::vector<std::array<uint16, 2>> Vertices;
		std::vector<uint16> Indices;
	};

	RENDERER_API auto IsValidTerrainTopologyKey(const FTerrainTopologyKey& Key) -> bool;
	RENDERER_API auto GetTerrainTopologyTriangleCount(const FTerrainTopologyKey& Key) -> size_t;
	RENDERER_API auto BuildTerrainTopology(
		const FTerrainTopologyKey& Key, FTerrainTopologyData& OutData) -> bool;
}

template <>
struct std::hash<Durin::FTerrainTopologyKey>
{
	auto operator()(const Durin::FTerrainTopologyKey& Key) const noexcept -> size_t
	{
		return static_cast<size_t>(Key.CellCountX)
			| (static_cast<size_t>(Key.CellCountY) << 8)
			| (static_cast<size_t>(Key.LODStep) << 16)
			| (static_cast<size_t>(Key.StitchMask) << 24);
	}
};
