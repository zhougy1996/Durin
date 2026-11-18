#pragma once

#include "CoreMinimal.h"

#include "Documents/DocumentDialogPresenters.h"
#include "Asset/Load.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class DLevel;
	class DWorld;
}

namespace Durin::LevelEditor
{
	class FEditorAssetMoveCoordinator;
	class FLevelEditorWorkspaceSettings;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	// Coordinates new/open/save/close flows and their confirmation popups.
	class FLevelDocumentController
	{
	public:
		FLevelDocumentController(
			FLevelEditorContext& InContext,
			FLevelEditorWorkspaceSettings& InWorkspaceSettings,
			FSceneViewportPanel& InSceneViewportPanel,
			FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
			std::function<void()> InLevelChanged,
			std::function<void()> InClearError,
			std::function<void(std::string)> InReportError
		);

		~FLevelDocumentController();
		auto RequestOpenProject(std::string ProjectFile) -> void;
		auto DrawDialogs() -> void;
		auto GetCurrentLevel() const -> DLevel*;
		auto GetCurrentLevelAsset() const -> DLevel*;
		auto IsCurrentLevelDirty() const -> bool;
		auto CanEditCurrentLevel() const -> bool;
		auto CanSaveCurrentLevel() const -> bool;
		auto SaveCurrentLevelAs(const FPackagePath& Path) -> bool;
		auto RequestSaveAs() -> void;
		auto RequestCloseLevel() -> bool;
		auto HasPendingTransition() const -> bool;
		auto SaveCurrentLevel() -> bool;
		auto RenameCurrentLevel(std::string_view NewName) -> bool;
		auto ReleaseLevelView() -> bool;

	private:
		friend class FLevelEditorToolkit;
		// Selects the modal confirmation that must be resolved before continuing.
		enum class EQueuedPopup
		{
			None,
			UnsavedLevel
		};

		auto ResolveUnsavedLevelDialog(EUnsavedLevelDialogDecision Decision) -> bool;
		auto ActivateCurrentLevel() -> void;
		auto CompletePendingTransition() -> bool;
		auto CloseCurrentLevel() -> bool;
		auto SetError(std::string Message) const -> void;

		FDelegateHandle MapChangedHandle;
		FLevelEditorContext& Context;
		FLevelEditorWorkspaceSettings& WorkspaceSettings;
		FSceneViewportPanel& SceneViewportPanel;
		FEditorAssetMoveCoordinator& AssetMoveCoordinator;
		std::function<void()> LevelChanged;
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		EQueuedPopup QueuedPopup = EQueuedPopup::None;
		std::string PendingPath;
		FObjectKey PendingLevel;
		bool bPendingCloseLevel = false;
		bool bOpenSaveAs = false;
		std::array<char, 512> SaveAsPath{};
		std::string SaveAsError;
		FUnsavedLevelDialogPresenter UnsavedLevelDialog;
		TWeakObjectPtr<DPackage> DocumentPackage;
	};
} // namespace Durin::LevelEditor
