#pragma once

#include "AssetForge/Operations/ImportOperation.h"

namespace Durin::Editor
{
	class FNotificationManager;
	class FTransactionManager;
}

namespace Durin::Editor::MainFrame
{
	// Identifies an editor-host status-bar tool action selected this frame.
	enum class EEditorStatusBarAction : uint8
	{
		None,
		ContentBrowser,
		Console,
		ActivityHistory,
	};

	// Draws active notifications and transaction feedback over the editor.
	class FEditorNotificationOverlay final
	{
	public:
		FEditorNotificationOverlay() = default;
		~FEditorNotificationOverlay();
		auto DrawHistoryWindow() -> void;
		auto UpdateNotifications(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void;
		auto GetStatusBarHeight() const -> float;
		auto DrawStatusBar(
			::Durin::Editor::FNotificationManager& Notifications,
			EEditorStatusBarAction SelectedDrawer,
			uint32 ConsoleUnreadCount) -> EEditorStatusBarAction;
		auto DrawToasts(::Durin::Editor::FNotificationManager& Notifications) -> void;
		auto OpenHistory() -> void;
		auto RegisterImportOperation(
			AssetForge::FImportOperationHandle Handle, std::string Title) -> void;

	private:
		struct FPresentedImportOperation
		{
			AssetForge::FImportOperationHandle Handle;
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
		bool bHistoryOpen = false;
	};
}
