#pragma once

#include "CoreMinimal.h"

#include "Documents/DocumentDialogPresenters.h"
#include "Asset/Load.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/HostStartup.h"
#include "Feedback/SlowTaskWidget.h"

namespace Durin
{
	class DLevel;
	class DWorld;
	struct FMapChangeProgress;
}

namespace Durin::Editor::Level
{
	class FEditorAssetMoveCoordinator;
	class FLevelEditorSessionSettings;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	// Identifies the pending level-document workflow requested by the user.
	enum class ELevelDocumentAction
	{
		None,
		OpenLevel,
		OpenProject
	};

	// Pending confirmation and loading are distinct; only Opened means activation completed.
	enum class ELevelDocumentOpenResult
	{
		Rejected,
		Opened,
		AwaitingConfirmation,
		Loading,
	};

	// Coordinates new/open/save/close flows and their confirmation popups.
	class FLevelDocumentController
	{
	public:
		FLevelDocumentController(
			FLevelEditorContext& InContext,
			FLevelEditorSessionSettings& InSessionSettings,
			FSceneViewportPanel& InSceneViewportPanel,
			FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
			TSoftObjectPtr<DLevel>& InDefaultLevel,
			std::function<void()> InClearError,
			std::function<void(std::string)> InReportError,
			std::function<void(bool)> InCompleteDeferredOpen
		);

		auto RequestOpenProject(std::string ProjectFile) -> void;
		// Interactive entry point: resolves unsaved changes before loading the selected level.
		auto OpenLevel(std::string Path) -> ELevelDocumentOpenResult;
		auto DrawDialogs() -> void;
		auto OpenDefaultLevel() -> EHostStartupState;
		auto CanSaveCurrentLevel() const -> bool;
		auto IsOpening() const -> bool;
		auto SaveCurrentLevel() -> bool;
		auto RenameCurrentLevel(std::string_view NewName) -> bool;
		auto CloseLevel() -> bool;

	private:
		// Selects the modal confirmation that must be resolved before continuing.
		enum class EQueuedPopup
		{
			None,
			UnsavedLevel
		};

		auto ExecutePendingAction() -> ELevelDocumentOpenResult;
		// No save prompt. Completion is delivered after activation by the ordinary host frame.
		auto LoadLevel(std::string_view Path, bool bShowProgress = true) -> ELevelDocumentOpenResult;
		auto ResolveUnsavedLevelDialog(EUnsavedLevelDialogDecision Decision) -> bool;
		auto CompletePendingDocumentOpen(bool bSucceeded) -> void;
		auto PrepareLevelActivation(DLevel& Level) -> bool;
		auto CompleteMapChange(const FMapChangeProgress& Progress) -> void;
		auto SetError(std::string Message) const -> void;

		FLevelEditorContext& Context;
		FLevelEditorSessionSettings& SessionSettings;
		FSceneViewportPanel& SceneViewportPanel;
		FEditorAssetMoveCoordinator& AssetMoveCoordinator;
		TSoftObjectPtr<DLevel>& DefaultLevel;
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(bool)> CompleteDeferredOpen;
		ELevelDocumentAction PendingAction = ELevelDocumentAction::None;
		EQueuedPopup QueuedPopup = EQueuedPopup::None;
		std::string PendingLevelPath;
		std::string PendingProjectFile;
		FUnsavedLevelDialogPresenter UnsavedLevelDialog;
		bool bShowLoadingProgress = false;
		::Durin::Editor::FSlowTaskDialogPresenter LoadingDialog;
		// Deferred completion must not borrow a controller after its document closes.
		std::shared_ptr<void> CallbackLifetime = std::make_shared<uint8>(0);
		bool bDefaultStarted = false;
		EHostStartupState DefaultOpenState = EHostStartupState::Pending;
		TWeakObjectPtr<DPackage> DocumentPackage;
		TWeakObjectPtr<DWorld> DocumentWorld;
		bool bPendingDocumentOpen = false;
	};
} // namespace Durin::Editor::Level
