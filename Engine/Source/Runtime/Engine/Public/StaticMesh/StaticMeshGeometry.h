#pragma once

#include "EngineAPI.h"

namespace Durin
{
	inline constexpr uint32 MaximumStaticMeshImportedUVChannels = 4;

	// Maps an imported material to its stable source identity.
	struct FStaticMeshImportedMaterialSlot
	{
		std::string Name;
		uint32 SourceMaterialIndex = 0;
		std::string SourceName;
	};

	// One detached imported section and its optional per-vertex channels.
	struct FStaticMeshImportedMesh
	{
		std::string Name;
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaximumStaticMeshImportedUVChannels> UVChannels;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
		uint32 SourceMaterialIndex = 0;
	};

	// Detached editable geometry; canonical storage publishes only immutable instances.
	struct FStaticMeshDecodedGeometry
	{
		std::vector<FStaticMeshImportedMaterialSlot> MaterialSlots;
		std::vector<FStaticMeshImportedMesh> Meshes;
	};

	// Owns decoded geometry independently of source replacement, release and asset lifetime.
	using FStaticMeshGeometryReadHandle = std::shared_ptr<const FStaticMeshDecodedGeometry>;
}
