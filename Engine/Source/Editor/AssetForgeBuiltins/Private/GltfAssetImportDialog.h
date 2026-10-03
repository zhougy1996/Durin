#pragma once

#include "CoreMinimal.h"

#include "Import/ImportDialogSupport.h"
#include "Import/MeshCoordinateImportModel.h"
#include "AssetForge/Builtins/AssetImport.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor
{
	// Imports glTF/GLB resources without a LevelEditor workspace.
	class FGltfAssetImportDialog
	{
	public:
		explicit FGltfAssetImportDialog(FImportDialogCallbacks InCallbacks = {});
		auto SetCallbacks(FImportDialogCallbacks InCallbacks) -> void { Callbacks = std::move(InCallbacks); }
		FGltfAssetImportDialog(const FGltfAssetImportDialog&) = delete;
		auto operator=(const FGltfAssetImportDialog&) -> FGltfAssetImportDialog& = delete;

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw(bool bAllowAssetMutation) -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestinationDirectory() -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;
		auto DrawOutputs() -> void;
		auto DrawMaterials(const FPackagePath& Directory, bool bCanPreview) -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDirectoryModel DestinationDirectory;
		FImportDialogModalState ModalState;
		std::array<char, 512> SourcePathBuffer{};
		FMeshCoordinateImportModel Coordinates;
		AssetForge::Builtins::FAssetImportOptions ImportOptions;
		std::vector<AssetForge::Builtins::FAssetImportSourceMesh> SourceMeshes;
		std::vector<AssetForge::Builtins::FAssetImportSourceScene> SourceScenes;
		AssetForge::Builtins::FSceneMaterialImportOptions MaterialOptions;
		AssetForge::Builtins::FAssetImportPreview MaterialPreview;
		std::array<char, 128> ParentSearch{};
		bool bPreviewDirty = true;
		std::unique_ptr<AssetForge::Builtins::FAssetImportSession> Session;
		bool bImporting = false;
		bool bReportedCompletion = false;
	};
} // namespace Durin::Editor
