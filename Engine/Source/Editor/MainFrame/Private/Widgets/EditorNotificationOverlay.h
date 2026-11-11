#pragma once

#include "CoreMinimal.h"
#include "DObject/ObjectKey.h"

namespace Durin
{
	class DTransactor;
}

namespace Durin
{
	class FNotificationManager;
	struct FTransactionEvent;
}

namespace Durin::MainFrame
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
		// Publishes transaction presentation feedback without requiring an ImGui frame.
		auto Tick(FNotificationManager& Notifications, DTransactor& Transactions) -> void;
		auto GetStatusBarHeight() const -> float;
		auto DrawStatusBar(
			FNotificationManager& Notifications,
			EEditorStatusBarAction SelectedDrawer,
			uint32 ConsoleUnreadCount) -> EEditorStatusBarAction;
		auto DrawToasts(FNotificationManager& Notifications) -> void;
		auto OpenHistory() -> void;

	private:
		static auto DrawHistory(FNotificationManager& Notifications, bool* bOpen) -> void;
		static auto PublishTransactionEvent(FNotificationManager& Notifications, DTransactor& Transactions, const FTransactionEvent& Event) -> void;
		FObjectKey TransactionSource;
		uint64 TransactionListener = 0;

		bool bFocusHistoryRequested = false;
		bool bHistoryOpen = false;
	};
}
