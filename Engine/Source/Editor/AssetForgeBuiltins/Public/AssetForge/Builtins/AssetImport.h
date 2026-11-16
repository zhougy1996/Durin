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
	class DAssetImportTask;
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
		auto operator==(const FAssetImportOptions&) const -> bool = default;
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
	struct FAssetImportResult
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


	enum class EAssetImportPhase : uint8
	{
		Reading, Ready, Building, Preparing, Compiling, Applying, Saving, Finishing, Completed
	};
	struct FAssetImportProgress
	{
		EAssetImportPhase Phase = EAssetImportPhase::Reading;
		std::string Activity;
		size_t Completed = 0;
		size_t Total = 0;
		bool bCancellationRequested = false;
	};

	// Feature-owned preview and progress host. Preparation does not publish assets;
	// ConfigureTask attaches its snapshot to the supported import command. Host
	// teardown cancels and drains accepted work at the owning safe point.
	struct FSceneImportState;
	class FSceneTaskOperation;
	class FSceneImportPreview final
	{
	public:
		ASSETFORGEBUILTINS_API FSceneImportPreview(std::string SourceFile,
			FPackagePath DestinationDirectory, FStaticMeshImportSettings Settings,
			FAssetImportOptions Options = {});
		ASSETFORGEBUILTINS_API ~FSceneImportPreview();
		FSceneImportPreview(const FSceneImportPreview&) = delete;
		auto operator=(const FSceneImportPreview&) -> FSceneImportPreview& = delete;
		ASSETFORGEBUILTINS_API auto Tick() -> void;
		ASSETFORGEBUILTINS_API auto Cancel() -> void;
		ASSETFORGEBUILTINS_API auto GetProgress() const -> const FAssetImportProgress&;
		ASSETFORGEBUILTINS_API auto GetResult() const -> const FAssetImportResult&;
		ASSETFORGEBUILTINS_API auto GetSourceMeshes() const -> std::vector<FAssetImportSourceMesh>;
		ASSETFORGEBUILTINS_API auto GetSourceScenes() const -> std::vector<FAssetImportSourceScene>;
		ASSETFORGEBUILTINS_API auto SetOptions(FAssetImportOptions Options) -> bool;
		// Resolves every selected output and material mapping from the captured source.
		ASSETFORGEBUILTINS_API auto PreviewOutputs(const FPackagePath& Destination,
			const FSceneMaterialImportOptions& Options = {}) -> FAssetImportPreview;
		// Populates task inputs with this prepared snapshot; execution still requires
		// IAssetTools::ImportAssetTasks. No direct scene execution entrypoint.
		ASSETFORGEBUILTINS_API auto ConfigureTask(DAssetImportTask& Task, const FPackagePath& Destination,
			FSceneMaterialImportOptions Options = {}) -> bool;
	private:
		friend class FSceneTaskOperation;
		explicit FSceneImportPreview(std::shared_ptr<FSceneImportState> State) : Impl(std::move(State)) {}
		auto ResolvePreview(const FPackagePath& Destination, const FSceneMaterialImportOptions& Options,
			bool bAllowAccepted) -> FAssetImportPreview;
		auto BeginImport(const FPackagePath& Destination, FSceneMaterialImportOptions Options,
			std::optional<FAssetPackageSaveArgs> SaveArgs) -> bool;
		std::shared_ptr<FSceneImportState> Impl;
	};

}
