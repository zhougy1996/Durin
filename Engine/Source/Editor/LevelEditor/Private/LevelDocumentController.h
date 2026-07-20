#pragma once

namespace Durin
{
	class DLevel;
	class FEditorAssetMoveCoordinator;
	class FEditorSessionSettings;
	struct FLevelEditorContext;
	class FSceneViewportPanel;

	enum class ELevelDocumentAction
	{
		None,
		OpenLevel,
		OpenProject
	};

	class FLevelDocumentController
	{
	public:
		FLevelDocumentController(
			FLevelEditorContext& InContext,
			FEditorSessionSettings& InSessionSettings,
			FSceneViewportPanel& InSceneViewportPanel,
			FEditorAssetMoveCoordinator& InAssetMoveCoordinator,
			std::string& InDefaultLevel,
			std::function<void()> InClearError,
			std::function<void(std::string)> InReportError
		);

		auto RequestAction(ELevelDocumentAction Action) -> void;
		auto RequestOpenLevel(std::string Path) -> bool;
		auto DrawDialogs() -> void;
		auto OpenDefaultLevel() -> void;
		auto SaveCurrentLevel() -> bool;
		auto RenameCurrentLevel(std::string_view NewName) -> bool;

	private:
		enum class EQueuedPopup
		{
			None,
			UnsavedLevel
		};

		auto ExecutePendingAction() -> void;
		auto OpenLevel(std::string_view Path) -> void;
		auto ActivateLevel(DLevel* Level) -> bool;
		auto SetError(std::string Message) const -> void;

		FLevelEditorContext& Context;
		FEditorSessionSettings& SessionSettings;
		FSceneViewportPanel& SceneViewportPanel;
		FEditorAssetMoveCoordinator& AssetMoveCoordinator;
		std::string& DefaultLevel;
		std::function<void()> ClearError;
		std::function<void(std::string)> ReportError;
		ELevelDocumentAction PendingAction = ELevelDocumentAction::None;
		EQueuedPopup QueuedPopup = EQueuedPopup::None;
		std::string PendingLevelPath;
	};
} // namespace Durin
