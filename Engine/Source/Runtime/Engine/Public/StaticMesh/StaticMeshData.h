#pragma once

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

	// Owns CPU vertex streams shared by recipe products and serialized LODs.
	struct FStaticMeshVertexData
	{
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaxStaticMeshUVChannels> TexCoords;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
	};

	// Detached LOD data; contains no render resources or GPU readiness state.
	struct FStaticMeshBuildLOD : FStaticMeshVertexData
	{
		std::vector<FStaticMeshSection> Sections;
		FBox LocalBounds;
		float ScreenSize = 0.0f;
		uint8 NumTexCoords = 0;
		bool bHasColorVertexData = false;
	};

	// Produces the deterministic policy used by builders without authored thresholds.
	ENGINE_API auto GenerateDefaultStaticMeshLODScreenSizes(
		uint32 LODCount) -> std::vector<float>;

}
