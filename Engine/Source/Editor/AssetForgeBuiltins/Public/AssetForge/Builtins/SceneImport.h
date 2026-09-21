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

	enum class ESceneImportPhase : uint8
	{
		Reading, Ready, Building, Preparing, Compiling, Saving, Finishing, Completed
	};
	struct FSceneImportProgress
	{
		ESceneImportPhase Phase = ESceneImportPhase::Reading;
		std::string Activity;
		size_t Completed = 0;
		size_t Total = 0;
		bool bCancellationRequested = false;
	};

	// GameThread-owned scene operation. Tick never waits for workers, compilation or
	// disk staging. The host must tick until completion; destruction cancels and
	// drains outstanding work at the host's shutdown safe point.
	class FSceneImportSession final
	{
	public:
		ASSETFORGEBUILTINS_API FSceneImportSession(std::string SourceFile,
			FPackagePath DestinationDirectory, FStaticMeshImportSettings Settings);
		ASSETFORGEBUILTINS_API ~FSceneImportSession();
		FSceneImportSession(const FSceneImportSession&) = delete;
		auto operator=(const FSceneImportSession&) -> FSceneImportSession& = delete;
		ASSETFORGEBUILTINS_API auto Tick() -> void;
		ASSETFORGEBUILTINS_API auto Cancel() -> void;
		ASSETFORGEBUILTINS_API auto GetProgress() const -> const FSceneImportProgress&;
		ASSETFORGEBUILTINS_API auto GetResult() const -> const FSceneImportResult&;
		ASSETFORGEBUILTINS_API auto PreviewMaterials(const FPackagePath& Destination,
			const FSceneMaterialImportOptions& Options) -> FSceneMaterialPreviewResult;
		ASSETFORGEBUILTINS_API auto BeginImport(const FPackagePath& Destination,
			FSceneMaterialImportOptions Options,
			FSceneImportPublicationOptions PublicationOptions = {}) -> bool;
	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		friend ASSETFORGEBUILTINS_API auto ImportSceneAssets(std::string_view, const FPackagePath&,
			const FStaticMeshImportSettings&, const std::function<bool()>&,
			const FSceneImportPublicationOptions&, const FSceneMaterialImportOptions&) -> FSceneImportResult;
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
