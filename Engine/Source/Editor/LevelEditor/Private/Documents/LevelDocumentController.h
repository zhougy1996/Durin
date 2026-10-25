#pragma once

#include "CoreMinimal.h"

#include "Documents/DocumentDialogPresenters.h"
#include "Asset/Load.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/HostStartup.h"

namespace Durin
{
	class DLevel;
	class DWorld;
	class FSceneLoadOperation;
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

	// Reports whether opening a level completed, failed, or awaits confirmation.
	enum class ELevelDocumentOpenResult
	{
		Rejected,
		Opened,
		Deferred,
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
		auto RequestOpenLevel(std::string Path) -> ELevelDocumentOpenResult;
		auto DrawDialogs() -> void;
		auto AdvancePendingOpen() -> void;
		auto OpenDefaultLevel() -> EHostStartupState;
		auto CanSaveCurrentLevel() const -> bool;
		auto IsOpening() const -> bool { return PendingSceneLoad != nullptr; }
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
		auto OpenLevel(std::string_view Path) -> ELevelDocumentOpenResult;
		auto ResolveUnsavedLevelDialog(EUnsavedLevelDialogDecision Decision) -> bool;
		auto CompletePendingDocumentOpen(bool bSucceeded) -> void;
		auto ActivateLevel(FSceneLoadOperation& Loading) -> bool;
		auto AdvanceOpening() -> EHostStartupState;
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
		std::shared_ptr<FSceneLoadOperation> ActiveSceneLoad, PendingSceneLoad;
		bool bDefaultStarted = false;
		EHostStartupState DefaultOpenState = EHostStartupState::Pending;
		TWeakObjectPtr<DPackage> DocumentPackage;
		TWeakObjectPtr<DWorld> DocumentWorld;
		bool bPendingDocumentOpen = false;
	};
} // namespace Durin::Editor::Level
