#pragma once

namespace Durin
{
	class DTransactor;
}

namespace Durin::Editor
{
	class FNotificationManager;
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
		auto DrawHistoryWindow() -> void;
		auto UpdateNotifications(::Durin::Editor::FNotificationManager& Notifications, ::Durin::DTransactor& Transactions) -> void;
		auto GetStatusBarHeight() const -> float;
		auto DrawStatusBar(
			::Durin::Editor::FNotificationManager& Notifications,
			EEditorStatusBarAction SelectedDrawer,
			uint32 ConsoleUnreadCount) -> EEditorStatusBarAction;
		auto DrawToasts(::Durin::Editor::FNotificationManager& Notifications) -> void;
		auto OpenHistory() -> void;

	private:
		static auto DrawHistory(::Durin::Editor::FNotificationManager& Notifications, bool* bOpen) -> void;
		static auto PublishTransactionEvents(::Durin::Editor::FNotificationManager& Notifications, ::Durin::DTransactor& Transactions) -> void;

		bool bFocusHistoryRequested = false;
		bool bHistoryOpen = false;
	};
}
