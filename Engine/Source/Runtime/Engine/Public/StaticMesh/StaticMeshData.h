#pragma once

#include "CoreMinimal.h"

#include "EngineAPI.h"
#include "Math/Box.h"

namespace Durin
{
	inline constexpr uint32 MaxStaticMeshUVChannels = 4;

	// Preserves index-ordered imported material metadata in runtime mesh data.
	struct FStaticMeshMaterialSlot
	{
		std::string Name;
		uint32 SourceMaterialIndex = 0;
	};

	// Describes one indexed draw range and its local-space bounds.
	struct FStaticMeshSection
	{
		std::string Name;
		uint32 FirstIndex = 0;
		uint32 IndexCount = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		uint32 MaterialSlotIndex = 0;
		FBox LocalBounds;
	};

	// Owns CPU vertex streams used by builder scratch and serialized LODs.
	struct FStaticMeshVertexData
	{
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> TexCoords;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
	};

	// Produces the deterministic policy used by builders without authored thresholds.
	ENGINE_API auto GenerateDefaultStaticMeshLODScreenSizes(
		uint32 LODCount) -> std::vector<float>;

}
