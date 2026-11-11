#pragma once

#include "CoreMinimal.h"

#include "Import/ImportDialogSupport.h"
#include "Import/MeshCoordinateImportModel.h"
#include "AssetForge/Builtins/AssetImport.h"

namespace Durin::LevelEditor
{
	// Imports one supported Scene source into a typed multi-asset directory.
	class FSceneImportDialog
	{
	public:
		explicit FSceneImportDialog(FImportDialogCallbacks InCallbacks);
		FSceneImportDialog(const FSceneImportDialog&) = delete;
		auto operator=(const FSceneImportDialog&) -> FSceneImportDialog& = delete;

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw(bool bAllowAssetMutation) -> void;

	private:
		auto BrowseSource() -> void;
		auto BrowseDestinationDirectory() -> void;
		auto Import() -> bool;
		auto SetError(std::string Message) const -> void;
		auto DrawMaterials(const FPackagePath& Directory, bool bCanPreview) -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDirectoryModel DestinationDirectory;
		FImportDialogModalState ModalState;
		std::array<char, 512> SourcePathBuffer{};
		FMeshCoordinateImportModel Coordinates;
		AssetForge::Builtins::FSceneMaterialImportOptions MaterialOptions;
		AssetForge::Builtins::FAssetImportPreview OutputPreview;
		std::array<char, 128> ParentSearch{};
		bool bPreviewDirty = true;
		std::unique_ptr<AssetForge::Builtins::FAssetImportSession> Session;
		bool bImporting = false;
		bool bReportedCompletion = false;
		bool bCloseWhenFinished = false;
	};
} // namespace Durin::LevelEditor
