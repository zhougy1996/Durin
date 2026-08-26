#pragma once

#include "Editor/Import/ImportDialogSupport.h"
#include "AssetForge/Builtins/SceneImport.h"
#include "AssetForge/Operations/ImportExecution.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::Level
{
	using namespace ::Durin::Editor::Import;
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
		auto BrowseSource() -> void;
		auto BrowseDestinationDirectory() -> void;
		auto BrowseSourceDestination() -> void;
		auto SuggestSourceDestination() -> void;
		auto RefreshPreview(const FAssetPath& DestinationDirectory) -> void;
		auto Import() -> bool;
		auto PollImport() -> bool;
		auto CancelRequests() -> void;
		auto SetError(std::string Message) const -> void;

		FImportDialogCallbacks Callbacks;
		FImportDialogDirectoryModel DestinationDirectory;
		FImportDialogModalState ModalState;
		FMountedSourceImportFormModel SourceForm;
		std::array<char, 512>& SourcePathBuffer = SourceForm.GetSourcePathBuffer();
		std::array<char, 512>& SourceDestinationBuffer = SourceForm.GetDestinationBuffer();
		FMeshCoordinateImportModel Coordinates;
		EMountedSourceImportMode& SourceMode = SourceForm.GetMode();
		std::string PreviewKey;
		std::optional<AssetForge::FImportResult> Preview;
		std::optional<AssetForge::FImportHandle> PreviewRequest;
		std::optional<AssetForge::Builtins::FSceneSourceBundleAsyncHandle> SourceRequest;
		std::optional<FAssetPath> PendingImportDirectory;
		std::optional<AssetForge::FImportHandle> ImportRequestHandle;
		FImportDialogProgressModel ImportProgress;
	};
} // namespace Durin::Editor::Level
