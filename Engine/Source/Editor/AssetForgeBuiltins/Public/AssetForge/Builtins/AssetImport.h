#pragma once

#include "CoreMinimal.h"
#include "Asset/PackageSerialization.h"

#include "AssetForge/Builtins/AssetImportTypes.h"
#include "AssetForgeBuiltinsAPI.h"
#include "StaticMesh/StaticMeshImportSettings.h"

namespace Durin
{
	class DMaterial;
	class DMaterialInstance;
	class DTexture2D;
}

namespace Durin::AssetForge::Builtins
{
	// glTF/GLB/FBX are one-time imports: occupied outputs conflict, and retained
	// receipts provide provenance without enabling source updates.
	inline constexpr std::string_view SceneImporterId = "Durin.Scene";

	enum class EAssetImportMeshMode : uint8 { Split, Combined };
	struct FAssetImportOptions
	{
		EAssetImportMeshMode MeshMode = EAssetImportMeshMode::Split;
		std::vector<uint32> SelectedMeshes;
		std::vector<uint32> SelectedScenes;
		bool bIncludeUnusedResources = false;
		bool bCreateMeshes = true;
		bool bCreateMaterials = true;
		bool bCreateTextures = true;
		static auto LegacyCombined() -> FAssetImportOptions
		{
			return {.MeshMode = EAssetImportMeshMode::Combined};
		}
	};

	struct FAssetImportSourceMesh
	{
		uint32 Index = 0;
		std::string Name;
		uint32 PrimitiveCount = 0;
		bool bSelected = false;
	};
	struct FAssetImportSourceScene
	{
		uint32 Index = 0;
		std::string Name;
		bool bDefault = false;
	};

	enum class ESceneMaterialImportMode : uint8 { CreateMaterials, CreateInstances, UseExisting };
	struct FSceneMaterialSelection
	{
		ESceneMaterialImportMode Mode = ESceneMaterialImportMode::CreateMaterials;
		std::string ParentMaterialPath;
		std::string ExistingMaterialPath;
		auto operator==(const FSceneMaterialSelection&) const -> bool = default;
	};
	struct FSceneMaterialOverride
	{
		std::string StableIdentity;
		FSceneMaterialSelection Selection;
		auto operator==(const FSceneMaterialOverride&) const -> bool = default;
	};
	struct FSceneMaterialImportOptions
	{
		FSceneMaterialSelection Default;
		std::vector<FSceneMaterialOverride> Overrides;
		auto operator==(const FSceneMaterialImportOptions&) const -> bool = default;
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
		// Import success describes the live graph, independently of optional persistence.
		std::vector<FPackagePath> ImportedPackages;
		std::optional<FAssetBatchSaveResult> SaveResult;
		std::vector<FImportOutputSummary> Outputs;
		std::vector<FImportDiagnostic> Diagnostics;
		std::string Message;

		explicit operator bool() const { return bSucceeded; }
	};
	struct FAssetImportPreview
	{
		bool bSucceeded = false;
		std::vector<FImportOutputSummary> Outputs;
		std::vector<FSceneMaterialPreview> Materials;
		std::vector<FImportDiagnostic> Diagnostics;
		std::string Message;
	};


	enum class ESceneImportPhase : uint8
	{
		Reading, Ready, Building, Preparing, Compiling, Applying, Saving, Finishing, Completed
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
	class FAssetImportSession final
	{
	public:
		ASSETFORGEBUILTINS_API FAssetImportSession(std::string SourceFile,
			FPackagePath DestinationDirectory, FStaticMeshImportSettings Settings,
			FAssetImportOptions Options = FAssetImportOptions::LegacyCombined());
		ASSETFORGEBUILTINS_API ~FAssetImportSession();
		FAssetImportSession(const FAssetImportSession&) = delete;
		auto operator=(const FAssetImportSession&) -> FAssetImportSession& = delete;
		ASSETFORGEBUILTINS_API auto Tick() -> void;
		ASSETFORGEBUILTINS_API auto Cancel() -> void;
		ASSETFORGEBUILTINS_API auto GetProgress() const -> const FSceneImportProgress&;
		ASSETFORGEBUILTINS_API auto GetResult() const -> const FSceneImportResult&;
		ASSETFORGEBUILTINS_API auto GetSourceMeshes() const -> std::vector<FAssetImportSourceMesh>;
		ASSETFORGEBUILTINS_API auto GetSourceScenes() const -> std::vector<FAssetImportSourceScene>;
		ASSETFORGEBUILTINS_API auto SetOptions(FAssetImportOptions Options) -> bool;
		ASSETFORGEBUILTINS_API auto PreviewOutputs(const FPackagePath& Destination,
			const FSceneMaterialImportOptions& Options = {}) -> FAssetImportPreview;
		ASSETFORGEBUILTINS_API auto PreviewMaterials(const FPackagePath& Destination,
			const FSceneMaterialImportOptions& Options) -> FSceneMaterialPreviewResult;
		// No save arguments means import to Dirty memory only; explicit arguments request ordinary saving.
		ASSETFORGEBUILTINS_API auto BeginImport(const FPackagePath& Destination,
			FSceneMaterialImportOptions Options,
			std::optional<FAssetPackageSaveArgs> SaveArgs = std::nullopt) -> bool;
	private:
		friend ASSETFORGEBUILTINS_API auto ImportAssetOutputs(std::string_view, const FPackagePath&,
			const FStaticMeshImportSettings&, const FAssetImportOptions&, const FSceneMaterialImportOptions&,
			const std::function<bool()>&, const std::optional<FAssetPackageSaveArgs>&) -> FSceneImportResult;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
		friend ASSETFORGEBUILTINS_API auto ImportSceneAssets(std::string_view, const FPackagePath&,
			const FStaticMeshImportSettings&, const std::function<bool()>&,
			const std::optional<FAssetPackageSaveArgs>&, const FSceneMaterialImportOptions&) -> FSceneImportResult;
	};

	using FSceneImportSession = FAssetImportSession;

	[[nodiscard]] ASSETFORGEBUILTINS_API auto ImportAssetOutputs(std::string_view SourceFile,
		const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings = FStaticMeshImportSettings::MakeYUpNegativeZForward(),
		const FAssetImportOptions& Options = {}, const FSceneMaterialImportOptions& Materials = {},
		const std::function<bool()>& IsCancellationRequested = {},
		const std::optional<FAssetPackageSaveArgs>& SaveArgs = std::nullopt) -> FSceneImportResult;

	// One-time combined import. Occupied destinations are rejected; mapped materials are preserved.
	[[nodiscard]] ASSETFORGEBUILTINS_API auto ImportSceneAssets(
		std::string_view SourceFile,
		const FPackagePath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		const std::function<bool()>& IsCancellationRequested = {},
		const std::optional<FAssetPackageSaveArgs>& SaveArgs = std::nullopt,
		const FSceneMaterialImportOptions& MaterialOptions = {}) -> FSceneImportResult;
}
