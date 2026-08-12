#pragma once

#include "ImportedScene.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin
{
	inline auto MakeStaticMeshImportedData(
		const Asset::FImportedSceneData& Scene)
		-> AssetBuild::FStaticMeshImportedData
	{
		AssetBuild::FStaticMeshImportedData Result;
		Result.MaterialSlots.reserve(Scene.MaterialSlots.size());
		for (const Asset::FImportedMaterialSlot& Slot : Scene.MaterialSlots)
		{
			Result.MaterialSlots.push_back({
				.Name = Slot.Name,
				.SourceMaterialIndex = Slot.SourceMaterialIndex,
				.SourceName = Slot.SourceName});
		}
		Result.Meshes.reserve(Scene.Meshes.size());
		for (const Asset::FImportedMeshData& Mesh : Scene.Meshes)
		{
			AssetBuild::FStaticMeshImportedMesh& Output = Result.Meshes.emplace_back();
			Output.Name = Mesh.Name;
			Output.Positions.assign(Mesh.Positions.begin(), Mesh.Positions.end());
			Output.Normals.assign(Mesh.Normals.begin(), Mesh.Normals.end());
			Output.Tangents.assign(Mesh.Tangents.begin(), Mesh.Tangents.end());
			for (uint32 Channel = 0;
				Channel < AssetBuild::MaximumStaticMeshImportedUVChannels;
				++Channel)
			{
				Output.UVChannels[Channel].assign(
					Mesh.UVChannels[Channel].begin(), Mesh.UVChannels[Channel].end());
			}
			Output.Colors.assign(Mesh.Colors.begin(), Mesh.Colors.end());
			Output.Indices = Mesh.Indices;
			Output.SourceMaterialIndex = Mesh.SourceMaterialIndex;
		}
		return Result;
	}
}
