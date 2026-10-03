#pragma once

#include "CoreMinimal.h"

#include "AssetForge/Builtins/ImportedScene.h"

namespace Durin::AssetForge::Builtins
{
	// A mesh resource owns its primitives once, independently of node instances.
	struct FImportedMeshResource
	{
		uint32 SourceMeshIndex = 0;
		std::string SourceName;
		std::vector<FImportedMeshData> Primitives;
	};

	struct FImportedTransformNode
	{
		uint32 SourceNodeIndex = 0;
		int32 ParentNodeIndex = -1;
		std::string SourceName;
		FMatrix4f LocalTransform{1.0f};
		FMatrix4f GlobalTransform{1.0f};
		// FBX nodes may reference several resources; glTF nodes reference at most one.
		std::vector<uint32> MeshIndices;
	};

	struct FImportedSourceScene
	{
		std::string SourceName;
		std::vector<uint32> RootNodeIndices;
	};

	// Geometry is in the requested engine basis, with source numeric lengths and
	// origin retained. Transforms are conjugated by that basis, never baked into
	// resource vertices. Scene roots select instances only for combined output.
	struct FImportedDocument
	{
		std::vector<FImportedMeshResource> Meshes;
		std::vector<FImportedTransformNode> Nodes;
		std::vector<FImportedSourceScene> Scenes;
		uint32 DefaultSceneIndex = 0;
		std::vector<FImportedMaterial> Materials;
		std::vector<FImportedImage> Images;
		std::vector<FImportedDependency> Dependencies;
		std::vector<FSceneImportDiagnostic> Diagnostics;
	};

	// Explicit compatibility projection for combined assets. Empty SceneIndices
	// selects the default source scene; no scene means no instances to expand.
	ASSETFORGEBUILTINS_API auto ExpandImportedDocument(const FImportedDocument& Document,
		FImportedSceneData& OutGeometry, std::string& OutError,
		std::span<const uint32> SceneIndices = {},
		std::span<const uint32> MeshIndices = {}) -> bool;

	ASSETFORGEBUILTINS_API auto SelectImportedMeshResource(const FImportedDocument& Document,
		uint32 MeshIndex, FImportedSceneData& OutGeometry, std::string& OutError) -> bool;

	ASSETFORGEBUILTINS_API auto ImportDocumentFromFile(std::string_view FilePath,
		FImportedDocument& OutDocument, const FMeshImportOptions& Options = {}) -> bool;
}
