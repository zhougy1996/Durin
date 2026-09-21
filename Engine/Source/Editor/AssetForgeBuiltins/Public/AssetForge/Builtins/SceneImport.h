#pragma once

#include "AssetForge/Builtins/SceneImportTypes.h"
#include "AssetForgeBuiltinsAPI.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin
{
	class DMaterial;
	class DMaterialInstance;
	class DTexture2D;
	enum class EAssetBundleSavePhase : uint8;
}

namespace Durin::AssetForge::Builtins
{
	inline constexpr std::string_view SceneImporterId = "Durin.Scene";

	enum class ESceneMaterialImportMode : uint8 { CreateMaterials, CreateInstances };
	struct FSceneMaterialSelection
	{
		ESceneMaterialImportMode Mode = ESceneMaterialImportMode::CreateMaterials;
		std::string ParentMaterialPath;
	};
	struct FSceneMaterialOverride
	{
		std::string StableIdentity;
		FSceneMaterialSelection Selection;
	};
	struct FSceneMaterialImportOptions
	{
		FSceneMaterialSelection Default;
		std::vector<FSceneMaterialOverride> Overrides;
		bool bRebuildExistingMaterials = false;
	};
	struct FSceneMaterialPreview
	{
		std::string StableIdentity;
		std::string SourceName;
		FPackagePath AssetPath;
		FSceneMaterialSelection Selection;
		bool bPreserved = false;
		bool bCompatible = false;
		std::string Message;
	};
	struct FSceneMaterialPreviewResult
	{
		bool bSucceeded = false;
		std::vector<FSceneMaterialPreview> Materials;
		std::string Message;
	};
	[[nodiscard]] ASSETFORGEBUILTINS_API auto PreviewSceneMaterials(
		std::string_view SourceFile, const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		const FSceneMaterialImportOptions& MaterialOptions = {}) -> FSceneMaterialPreviewResult;

	struct FSceneImportResult
	{
		bool bSucceeded = false;
		bool bPersisted = false;
		// Includes committed outputs, even when a later package fails.
		std::vector<FPackagePath> SavedPackages;
		std::vector<FImportOutputSummary> Outputs;
		std::vector<FImportDiagnostic> Diagnostics;
		std::string Message;

		explicit operator bool() const { return bSucceeded; }
	};
	struct FSceneImportPublicationOptions
	{
		std::function<bool(EAssetBundleSavePhase, size_t)> ShouldFail;
	};

	// Matches source/output identities; preserves materials and mesh bindings unless
	// explicitly rebuilding them. Unrelated assets and selected parents are never overwritten.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ImportSceneAssets(
		std::string_view SourceFile,
		const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		const std::function<bool()>& IsCancellationRequested = {},
		const FSceneImportPublicationOptions& PublicationOptions = {},
		const FSceneMaterialImportOptions& MaterialOptions = {}) -> FSceneImportResult;
}
