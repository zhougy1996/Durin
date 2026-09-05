#pragma once

#include "AssetForge/Builtins/SceneImportTypes.h"
#include "AssetForgeBuiltinsAPI.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	class DMaterial;
	class DMaterialInstance;
	class DTexture2D;
}

namespace Durin::AssetForge::Builtins
{
	inline constexpr std::string_view ImportedSurfaceMaterialPackagePath =
		"/Engine/Materials/ImportedSurface";
	inline constexpr std::string_view ImportedSurfaceMaterialObjectPath =
		"/Engine/Materials/ImportedSurface.ImportedSurface";
	inline constexpr std::string_view SceneImporterId = "Durin.Scene";

	struct FSceneImportResult
	{
		bool bSucceeded = false;
		bool bPersisted = false;
		std::vector<FImportOutputSummary> Outputs;
		std::vector<FImportDiagnostic> Diagnostics;
		std::string Message;

		explicit operator bool() const { return bSucceeded; }
	};

	ASSETFORGEBUILTINS_API auto ImportSceneAssets(
		std::string_view SourceFile,
		const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		FSceneImportResult& OutResult,
		const std::function<bool()>& IsCancellationRequested = {}) -> bool;

	ASSETFORGEBUILTINS_API auto EnsureImportedSurfaceMaterial(
		std::string& OutError) -> DMaterial*;
}
