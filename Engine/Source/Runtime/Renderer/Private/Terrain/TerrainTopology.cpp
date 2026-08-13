#include "Terrain/TerrainTopology.h"

namespace Durin
{
	namespace
	{
		constexpr uint8 AllStitchEdges = 0x0f;

		auto HasEdge(uint8 Mask, ETerrainStitchEdge Edge) -> bool
		{
			return (Mask & static_cast<uint8>(Edge)) != 0;
		}
	}

	auto IsValidTerrainTopologyKey(const FTerrainTopologyKey& Key) -> bool
	{
		if (Key.CellCountX == 0 || Key.CellCountY == 0 || Key.CellCountX > 64
			|| Key.CellCountY > 64 || Key.LODStep == 0 || Key.LODStep > 64
			|| (Key.LODStep & (Key.LODStep - 1)) != 0
			|| Key.CellCountX % Key.LODStep != 0
			|| Key.CellCountY % Key.LODStep != 0
			|| (Key.StitchMask & ~AllStitchEdges) != 0) return false;
		const uint32 CellsX = Key.CellCountX / Key.LODStep;
		const uint32 CellsY = Key.CellCountY / Key.LODStep;
		if ((HasEdge(Key.StitchMask, ETerrainStitchEdge::North)
				|| HasEdge(Key.StitchMask, ETerrainStitchEdge::South))
			&& CellsX % 2 != 0) return false;
		if ((HasEdge(Key.StitchMask, ETerrainStitchEdge::East)
				|| HasEdge(Key.StitchMask, ETerrainStitchEdge::West))
			&& CellsY % 2 != 0) return false;
		return true;
	}

	auto GetTerrainTopologyTriangleCount(const FTerrainTopologyKey& Key) -> size_t
	{
		if (!IsValidTerrainTopologyKey(Key)) return 0;
		const size_t CellsX = Key.CellCountX / Key.LODStep;
		const size_t CellsY = Key.CellCountY / Key.LODStep;
		size_t Result = CellsX * CellsY * 2;
		if (HasEdge(Key.StitchMask, ETerrainStitchEdge::North)) Result -= CellsX / 2;
		if (HasEdge(Key.StitchMask, ETerrainStitchEdge::South)) Result -= CellsX / 2;
		if (HasEdge(Key.StitchMask, ETerrainStitchEdge::East)) Result -= CellsY / 2;
		if (HasEdge(Key.StitchMask, ETerrainStitchEdge::West)) Result -= CellsY / 2;
		// Both edges collapse toward their preceding coarse coordinate; their
		// south-east corner additionally makes one interior diagonal collinear.
		if (HasEdge(Key.StitchMask, ETerrainStitchEdge::East)
			&& HasEdge(Key.StitchMask, ETerrainStitchEdge::South)) --Result;
		return Result;
	}

	auto BuildTerrainTopology(
		const FTerrainTopologyKey& Key, FTerrainTopologyData& OutData) -> bool
	{
		OutData = {};
		if (!IsValidTerrainTopologyKey(Key)) return false;
		const uint16 CellsX = Key.CellCountX / Key.LODStep;
		const uint16 CellsY = Key.CellCountY / Key.LODStep;
		const uint16 Width = CellsX + 1;
		OutData.Vertices.reserve(static_cast<size_t>(Width) * (CellsY + 1));
		for (uint16 Y = 0; Y <= CellsY; ++Y)
			for (uint16 X = 0; X <= CellsX; ++X)
				OutData.Vertices.push_back({
					static_cast<uint16>(X * Key.LODStep),
					static_cast<uint16>(Y * Key.LODStep)});

		auto Remap = [&](uint16 X, uint16 Y) -> uint16 {
			if (Y == 0 && HasEdge(Key.StitchMask, ETerrainStitchEdge::North) && (X & 1u)) --X;
			if (Y == CellsY && HasEdge(Key.StitchMask, ETerrainStitchEdge::South) && (X & 1u)) --X;
			if (X == CellsX && HasEdge(Key.StitchMask, ETerrainStitchEdge::East) && (Y & 1u)) --Y;
			if (X == 0 && HasEdge(Key.StitchMask, ETerrainStitchEdge::West) && (Y & 1u)) --Y;
			return static_cast<uint16>(Y * Width + X);
		};
		auto AddTriangle = [&](uint16 A, uint16 B, uint16 C) -> bool {
			if (A >= OutData.Vertices.size() || B >= OutData.Vertices.size()
				|| C >= OutData.Vertices.size()) return false;
			const auto& VA = OutData.Vertices[A];
			const auto& VB = OutData.Vertices[B];
			const auto& VC = OutData.Vertices[C];
			const int32 Area = (static_cast<int32>(VB[0]) - VA[0])
				* (static_cast<int32>(VC[1]) - VA[1])
				- (static_cast<int32>(VB[1]) - VA[1])
				* (static_cast<int32>(VC[0]) - VA[0]);
			if (Area > 0) OutData.Indices.insert(OutData.Indices.end(), {A, B, C});
			else if (Area < 0) OutData.Indices.insert(OutData.Indices.end(), {A, C, B});
			return true;
		};
		OutData.Indices.reserve(GetTerrainTopologyTriangleCount(Key) * 3);
		for (uint16 Y = 0; Y < CellsY; ++Y)
			for (uint16 X = 0; X < CellsX; ++X)
			{
				const uint16 A = Remap(X, Y);
				const uint16 B = Remap(X + 1, Y);
				const uint16 C = Remap(X, Y + 1);
				const uint16 D = Remap(X + 1, Y + 1);
				if (!AddTriangle(A, B, C) || !AddTriangle(B, D, C))
				{
					OutData = {};
					return false;
				}
			}
		if (OutData.Indices.size() / 3 != GetTerrainTopologyTriangleCount(Key))
		{
			OutData = {};
			return false;
		}
		return true;
	}
}
