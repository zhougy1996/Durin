#pragma once

#include "Panels/LevelEditorPanel.h"

namespace Durin::Editor
{
	class FNotificationManager;
	class FTransactionManager;
}

namespace Durin::Editor::Level
{
	// Draws active notifications and transaction feedback over the editor.
	class FEditorNotificationOverlay final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "Activity History"; }
		auto Draw(FLevelEditorContext& Context) -> void override;
		auto UpdateNotifications(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void;
		auto GetStatusBarHeight() const -> float;
		auto DrawStatusBar(::Durin::Editor::FNotificationManager& Notifications) -> void;
		auto DrawToasts(::Durin::Editor::FNotificationManager& Notifications) -> void;

	private:
		static auto DrawHistory(::Durin::Editor::FNotificationManager& Notifications, bool* bOpen) -> void;
		static auto PublishTransactionEvents(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void;

		bool bFocusHistoryRequested = false;
	};
}
