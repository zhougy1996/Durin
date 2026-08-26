#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "AssetForge/ImportRequest.h"
#include "AssetForge/Operations/ImportExecution.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::AssetForge::Builtins
{
	ASSETFORGEBUILTINS_API auto InspectStaticMeshSource(
		const DStaticMesh& Mesh) -> FStaticMeshSourceDiagnostic;
	ASSETFORGEBUILTINS_API auto MakeStaticMeshImportRequest(
		const FSourcePath& MountedSource,
		const FAssetPath& Destination,
		const FStaticMeshImportSettings& Settings,
		EImportMode Mode,
		FImportOperationOwner Owner,
		std::optional<FImportProvenance> ExistingProvenance,
		FImportRequest& OutRequest,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto InspectStaticMeshImportProvenance(
		const DStaticMesh& Mesh,
		FImportProvenance& OutProvenance,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto SubmitStaticMeshImport(
		std::string_view FilePath, const FAssetPath& Destination,
		const FStaticMeshImportSettings& Settings,
		std::string_view SourceDestination, bool bAllowEngineContentWrite,
		FImportCompletion Completion,
		std::string& OutError) -> FImportHandle;
	ASSETFORGEBUILTINS_API auto ChangeStaticMeshSourceReference(
		DStaticMesh& Mesh,
		std::string_view SourceVirtualPath,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto IngestAndChangeStaticMeshSource(
		DStaticMesh& Mesh,
		std::string_view FilePath,
		std::string_view TargetSourceVirtualPath,
		std::string& OutError) -> bool;
	ASSETFORGEBUILTINS_API auto CreateTransientStaticMeshFromFile(
		std::string_view FilePath,
		DObject* Outer,
		std::string_view ObjectName,
		std::string& OutError,
		const FStaticMeshImportSettings& ImportSettings = {}) -> DStaticMesh*;
	ASSETFORGEBUILTINS_API auto ImportStaticMeshAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FStaticMeshImportSettings& ImportSettings = {},
		std::string_view SourceDestination = {},
		bool bAllowEngineContentWrite = false) -> FStaticMeshImportResult;
}
