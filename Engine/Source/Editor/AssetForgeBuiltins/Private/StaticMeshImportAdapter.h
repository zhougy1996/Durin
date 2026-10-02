#pragma once

#include "AssetForge/Builtins/ImportedScene.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshAttributes.h"

namespace Durin::AssetForge::Builtins
{
	inline auto MakeMeshDescription(
		const FImportedSceneData& Scene)
		-> FMeshDescription
	{
		FMeshDescription Result;
		FStaticMeshAttributes Attributes(Result);
		Result.PolygonGroups.reserve(Scene.MaterialSlots.size());
		for (const FImportedMaterialSlot& Slot : Scene.MaterialSlots)
		{
			Result.PolygonGroups.push_back({
				.Name = Slot.Name,
				.SourceMaterialIndex = Slot.SourceMaterialIndex,
				.SourceName = Slot.SourceName});
		}
		Result.Sections.reserve(Scene.Meshes.size());
		for (const FImportedMeshData& Mesh : Scene.Meshes)
		{
			FMeshDescriptionSection& Output = Result.Sections.emplace_back();
			Output.Name = Mesh.Name;
			Attributes.GetVertexPositions(Result.Sections.size() - 1).assign(Mesh.Positions.begin(), Mesh.Positions.end());
			Attributes.GetVertexInstanceNormals(Result.Sections.size() - 1).assign(Mesh.Normals.begin(), Mesh.Normals.end());
			Attributes.GetVertexInstanceTangents(Result.Sections.size() - 1).assign(Mesh.Tangents.begin(), Mesh.Tangents.end());
			for (uint32 Channel = 0;
				Channel < MaximumMeshDescriptionUVChannels;
				++Channel)
			{
				Attributes.GetVertexInstanceUVs(Result.Sections.size() - 1, Channel).assign(
					Mesh.UVChannels[Channel].begin(), Mesh.UVChannels[Channel].end());
			}
			Attributes.GetVertexInstanceColors(Result.Sections.size() - 1).assign(Mesh.Colors.begin(), Mesh.Colors.end());
			Output.Indices = Mesh.Indices;
			Output.SourceMaterialIndex = Mesh.SourceMaterialIndex;
		}
		return Result;
	}
}
