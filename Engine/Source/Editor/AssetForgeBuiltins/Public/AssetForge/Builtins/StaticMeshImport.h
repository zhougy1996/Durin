#pragma once

#include "AssetForgeBuiltinsAPI.h"
#include "Asset/PackageSerialization.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::AssetForge::Builtins
{
	ASSETFORGEBUILTINS_API auto ReimportStaticMesh(
		DStaticMesh& Mesh,
		std::string& OutError,
		const Asset::FAssetBundleSaveOptions& SaveOptions = {}) -> bool;
	ASSETFORGEBUILTINS_API auto ReimportStaticMeshFromFile(
		DStaticMesh& Mesh,
		std::string_view FilePath,
		std::string& OutError,
		const Asset::FAssetBundleSaveOptions& SaveOptions = {}) -> bool;
	ASSETFORGEBUILTINS_API auto CreateTransientStaticMeshFromFile(
		std::string_view FilePath,
		DObject* Outer,
		std::string_view ObjectName,
		std::string& OutError,
		const FStaticMeshImportSettings& ImportSettings = {}) -> DStaticMesh*;
	ASSETFORGEBUILTINS_API auto ImportStaticMeshAsset(
		std::string_view FilePath,
		std::string_view AssetPath,
		const FStaticMeshImportSettings& ImportSettings = {})
		-> FStaticMeshImportResult;
}
