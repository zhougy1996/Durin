#pragma once

#include "CoreMinimal.h"
#include "AssetImportTask.h"
#include "DObject/StrongObjectPtr.h"

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
		~FSceneImportDialog();
		FSceneImportDialog(const FSceneImportDialog&) = delete;
		auto operator=(const FSceneImportDialog&) -> FSceneImportDialog& = delete;

		auto Open(std::string_view DestinationDirectory = {}) -> void;
		auto Draw(bool bAllowAssetMutation) -> void;

	private:
		auto ResetSession() -> void;
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
		std::unique_ptr<AssetForge::Builtins::FSceneImportPreview> Session;
		TStrongObjectPtr<DAssetImportTask> Task;
		bool bImporting = false;
		bool bReportedCompletion = false;
		bool bCloseWhenFinished = false;
	};
} // namespace Durin::LevelEditor
