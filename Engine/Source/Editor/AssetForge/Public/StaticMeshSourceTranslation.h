#pragma once

#include "AssetForgeAPI.h"
#include "Interchange.h"
#include "InterchangeJob.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Asset::Forge
{
	ASSETFORGE_API auto InspectStaticMeshSource(
		const DStaticMesh& Mesh) -> FStaticMeshSourceDiagnostic;
	ASSETFORGE_API auto MakeStaticMeshInterchangeRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		const FStaticMeshImportSettings& Settings,
		EInterchangeImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FInterchangeProvenance> ExistingProvenance,
		FInterchangeImportRequest& OutRequest,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto InspectStaticMeshInterchangeProvenance(
		const DStaticMesh& Mesh,
		FInterchangeProvenance& OutProvenance,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto SubmitStaticMeshInterchangeImport(
		std::string_view FilePath, const FAssetPath& Destination,
		const FStaticMeshImportSettings& Settings,
		std::string_view SourceDestination, bool bEngineAuthoringContext,
		FInterchangeImportCompletion Completion,
		std::string& OutError) -> FInterchangeImportHandle;
	ASSETFORGE_API auto ChangeStaticMeshSourceReference(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto IngestAndChangeStaticMeshSource(
		DStaticMesh& Mesh,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		std::string& OutError) -> bool;
	ASSETFORGE_API auto CreateTransientStaticMeshFromFile(
		std::string_view FilePath,
		DObject* Outer,
		std::string_view ObjectName,
		std::string& OutError,
		const FStaticMeshImportSettings& ImportSettings = {}) -> DStaticMesh*;
	ASSETFORGE_API auto ImportStaticMeshAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FStaticMeshImportSettings& ImportSettings = {},
		std::string_view SourceDestination = {},
		bool bEngineAuthoringContext = false) -> FStaticMeshImportResult;
}
