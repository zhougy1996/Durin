#pragma once

#include "Assets/ImportDialogState.h"
#include "Assets/MountedSourceImport.h"
#include "SceneImport.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::Level
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
		auto Draw() -> void;

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
		std::optional<Asset::Forge::FSceneImportPlanResult> Preview;
		std::optional<Asset::Forge::FSceneImportAsyncPlanHandle> PreviewRequest;
		std::optional<Asset::Forge::FSceneSourceBundleAsyncHandle> SourceRequest;
		std::optional<FAssetPath> PendingImportDirectory;
		std::optional<Asset::Forge::FSceneImportAsyncPlanHandle> ImportRequest;
		std::optional<Asset::Forge::FSceneImportAsyncExecutionHandle> ExecutionRequest;
		std::optional<Asset::Forge::FSceneImportPlan> ActiveImportPlan;
		std::optional<Asset::FAsyncImportPlanHandle> ActiveImportOperation;
		FImportDialogProgressModel ImportProgress;
	};
} // namespace Durin::Editor::Level
