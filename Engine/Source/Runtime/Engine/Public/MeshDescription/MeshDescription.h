#pragma once

#include "CoreMinimal.h"

#include "Misc/CoreTypes.h"
#include "Math/Vector.h"

namespace Durin
{
	inline constexpr uint32 MaximumMeshDescriptionUVChannels = 4;

	// Material groups use source-stable identities; asset material objects are not geometry.
	struct FMeshPolygonGroup
	{
		std::string Name;
		uint32 SourceMaterialIndex = 0;
		std::string SourceName;
	};

	// Triangle topology is local to a section. Indices address vertex instances,
	// whose attributes can differ even when they share a geometric vertex.
	struct FMeshDescriptionSection
	{
		std::string Name;
		std::vector<FVector3f> Positions;
		std::vector<FVector3f> Normals;
		std::vector<FVector4f> Tangents;
		std::array<std::vector<FVector2f>, MaximumMeshDescriptionUVChannels> UVChannels;
		std::vector<FVector4f> Colors;
		std::vector<uint32> Indices;
		uint32 SourceMaterialIndex = 0;
		// Empty means one instance per vertex. This compact form preserves v1 source bytes.
		std::vector<uint32> VertexInstanceVertices;

		auto GetVertexInstanceCount() const -> size_t
		{ return VertexInstanceVertices.empty() ? Positions.size() : VertexInstanceVertices.size(); }
		auto GetVertexIndex(uint32 Instance) const -> uint32
		{ return VertexInstanceVertices.empty() ? Instance : VertexInstanceVertices.at(Instance); }
	};

	// Editable source topology. Source residency publishes shared immutable snapshots.
	// This triangle/section model does not implement UE's edge or arbitrary polygon APIs.
	struct FMeshDescription
	{
		std::vector<FMeshPolygonGroup> PolygonGroups;
		std::vector<FMeshDescriptionSection> Sections;
	};

	using FMeshDescriptionReadHandle = std::shared_ptr<const FMeshDescription>;
}
