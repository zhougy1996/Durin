#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	class DTransactor;
}

namespace Durin::Editor
{
	class FNotificationManager;
	struct FTransactionEvent;
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
		static auto PublishTransactionEvent(::Durin::Editor::FNotificationManager& Notifications, ::Durin::DTransactor& Transactions, const ::Durin::Editor::FTransactionEvent& Event) -> void;
		FObjectKey TransactionSource;
		uint64 TransactionListener = 0;

		bool bFocusHistoryRequested = false;
		bool bHistoryOpen = false;
	};
}
