#pragma once

#include "MeshDescription/MeshDescription.h"

#include <span>

namespace Durin
{
	// Borrowed attribute view; it never owns a second geometry allocation.
	class FStaticMeshAttributes
	{
	public:
		explicit FStaticMeshAttributes(FMeshDescription& InDescription) : Description(InDescription) {}
		auto GetVertexPositions(size_t Section) -> std::vector<FVector3f>&
		{ return Description.Sections.at(Section).Positions; }
		auto GetVertexInstanceNormals(size_t Section) -> std::vector<FVector3f>&
		{ return Description.Sections.at(Section).Normals; }
		auto GetVertexInstanceTangents(size_t Section) -> std::vector<FVector4f>&
		{ return Description.Sections.at(Section).Tangents; }
		auto GetVertexInstanceUVs(size_t Section, size_t Channel) -> std::vector<FVector2f>&
		{ return Description.Sections.at(Section).UVChannels.at(Channel); }
		auto GetVertexInstanceColors(size_t Section) -> std::vector<FVector4f>&
		{ return Description.Sections.at(Section).Colors; }
		auto GetPolygonGroups() -> std::vector<FMeshPolygonGroup>& { return Description.PolygonGroups; }
	private:
		FMeshDescription& Description;
	};

	class FStaticMeshConstAttributes
	{
	public:
		explicit FStaticMeshConstAttributes(const FMeshDescription& InDescription) : Description(InDescription) {}
		auto GetVertexPositions(size_t Section) const -> std::span<const FVector3f>
		{ return Description.Sections.at(Section).Positions; }
		auto GetVertexInstanceNormals(size_t Section) const -> std::span<const FVector3f>
		{ return Description.Sections.at(Section).Normals; }
		auto GetVertexInstanceTangents(size_t Section) const -> std::span<const FVector4f>
		{ return Description.Sections.at(Section).Tangents; }
		auto GetVertexInstanceUVs(size_t Section, size_t Channel) const -> std::span<const FVector2f>
		{ return Description.Sections.at(Section).UVChannels.at(Channel); }
		auto GetVertexInstanceColors(size_t Section) const -> std::span<const FVector4f>
		{ return Description.Sections.at(Section).Colors; }
		auto GetPolygonGroups() const -> std::span<const FMeshPolygonGroup> { return Description.PolygonGroups; }
	private:
		const FMeshDescription& Description;
	};
}
