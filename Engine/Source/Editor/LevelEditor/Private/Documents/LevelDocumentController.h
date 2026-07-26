#pragma once

#include "AssetSystem.h"

namespace Durin
{
	class DLevel;
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
			std::string& InDefaultLevel,
			std::function<void()> InClearError,
			std::function<void(std::string)> InReportError,
			std::function<void(bool)> InCompleteDeferredOpen
		);

		auto RequestAction(ELevelDocumentAction Action) -> void;
		auto RequestOpenLevel(std::string Path) -> ELevelDocumentOpenResult;
		auto DrawDialogs() -> void;
		auto OpenDefaultLevel() -> void;
		auto SaveCurrentLevel() -> bool;
		auto RenameCurrentLevel(std::string_view NewName) -> bool;

	private:
		// Selects the modal confirmation that must be resolved before continuing.
		enum class EQueuedPopup
		{
			None,
			UnsavedLevel,
			AssetStructureUpgrade
		};

		auto ExecutePendingAction() -> ELevelDocumentOpenResult;
		auto OpenLevel(std::string_view Path) -> ELevelDocumentOpenResult;
		auto DrawUnsavedLevelDialog() -> void;
		auto DrawAssetStructureUpgradeDialog() -> void;
		auto SaveAndActivatePendingLevel(bool bAllowCompatibilityDataLoss) -> bool;
		auto ActivatePendingLevel() -> bool;
		auto CancelPendingLevelOpen() -> void;
		auto ResetPendingLevelState(bool bUnloadPackage) -> void;
		auto CompletePendingDocumentOpen(bool bSucceeded) -> void;
		auto ActivateLevel(DLevel* Level) -> bool;
		auto SetError(std::string Message) const -> void;

		FLevelEditorContext& Context;
		FLevelEditorSessionSettings& SessionSettings;
		FSceneViewportPanel& SceneViewportPanel;
		FEditorAssetMoveCoordinator& AssetMoveCoordinator;
		std::string& DefaultLevel;
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		std::function<void(bool)> CompleteDeferredOpen;
		ELevelDocumentAction PendingAction = ELevelDocumentAction::None;
		EQueuedPopup QueuedPopup = EQueuedPopup::None;
		std::string PendingLevelPath;
		DLevel* PendingLoadedLevel = nullptr;
		Asset::FAssetLoadReport PendingLoadReport;
		bool bPendingDocumentOpen = false;
		bool bCompatibilityDataLossConfirmed = false;
	};
} // namespace Durin
