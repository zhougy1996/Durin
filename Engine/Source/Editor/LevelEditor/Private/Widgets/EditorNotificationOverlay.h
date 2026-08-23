#pragma once

#include "AsyncImport.h"
#include "Panels/LevelEditorPanel.h"
#include "Workspace/LevelEditorPresentationPolicy.h"

namespace Durin::Editor
{
	class FNotificationManager;
	class FTransactionManager;
}

namespace Durin::Editor::Level
{
	// Identifies a Level Editor status-bar tool action selected this frame.
	enum class EEditorStatusBarAction : uint8
	{
		None,
		ContentBrowser,
		Console,
		ActivityHistory,
	};

	// Draws active notifications and transaction feedback over the editor.
	class FEditorNotificationOverlay final : public ILevelEditorPanel
	{
	public:
		FEditorNotificationOverlay()
			: ILevelEditorPanel(IsLevelEditorPanelOpenByDefault(
				ELevelEditorPanelRole::ActivityHistory))
		{
		}
		~FEditorNotificationOverlay() override;
		auto GetWindowName() const -> const char* override { return "Activity History"; }
		auto Draw(FLevelEditorContext& Context) -> void override;
		auto UpdateNotifications(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void;
		auto GetStatusBarHeight() const -> float;
		auto DrawStatusBar(
			::Durin::Editor::FNotificationManager& Notifications,
			EEditorStatusBarAction SelectedDrawer,
			uint32 ConsoleUnreadCount) -> EEditorStatusBarAction;
		auto DrawToasts(::Durin::Editor::FNotificationManager& Notifications) -> void;
		auto OpenHistory() -> void;
		auto RegisterImportOperation(
			Asset::FImportOperationHandle Handle, std::string Title) -> void;

	private:
		struct FPresentedImportOperation
		{
			Asset::FImportOperationHandle Handle;
			std::string Title;
			uint64 LastRevision = 0;
			uint64 NotificationId = 0;
		};

		auto UpdateImportOperations(
			::Durin::Editor::FNotificationManager& Notifications) -> void;
		static auto DrawHistory(::Durin::Editor::FNotificationManager& Notifications, bool* bOpen) -> void;
		static auto PublishTransactionEvents(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void;

		bool bFocusHistoryRequested = false;
		std::vector<FPresentedImportOperation> ImportOperations;
		uint64 ImportAggregateNotificationId = 0;
	};
}
