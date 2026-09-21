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

	struct FSceneImportResult
	{
		bool bSucceeded = false;
		bool bPersisted = false;
		// Includes committed generated parents and outputs, even when a later package fails.
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

	// Reimports matching source/output identities from source, discarding edits to
	// generated outputs. Unrelated assets and shared structural parents are never overwritten.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ImportSceneAssets(
		std::string_view SourceFile,
		const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		const std::function<bool()>& IsCancellationRequested = {},
		const FSceneImportPublicationOptions& PublicationOptions = {}) -> FSceneImportResult;
}
