#pragma once

#include "StandardAssetImportAPI.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Asset::Import
{
	STANDARDASSETIMPORT_API auto InspectStaticMeshSource(
		const DStaticMesh& Mesh) -> FStaticMeshSourceDiagnostic;
	STANDARDASSETIMPORT_API auto ChangeStaticMeshSourceReference(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto IngestAndChangeStaticMeshSource(
		DStaticMesh& Mesh,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		std::string& OutError) -> bool;
	STANDARDASSETIMPORT_API auto CreateTransientStaticMeshFromFile(
		std::string_view FilePath,
		DObject* Outer,
		std::string_view ObjectName,
		std::string& OutError,
		const FStaticMeshImportSettings& ImportSettings = {}) -> DStaticMesh*;
	STANDARDASSETIMPORT_API auto ImportStaticMeshAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FStaticMeshImportSettings& ImportSettings = {},
		std::string_view SourceDestination = {},
		bool bEngineAuthoringContext = false) -> FStaticMeshImportResult;
}
