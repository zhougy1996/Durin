#pragma once

#include "Panels/LevelEditorPanel.h"

namespace Durin::Editor
{
	class FTransactionManager;
}

namespace Durin
{
	class FEditorNotificationManager;

	// Draws active notifications and transaction feedback over the editor.
	class FEditorNotificationOverlay final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "Activity History"; }
		auto Draw(FLevelEditorContext& Context) -> void override;
		auto DrawNotifications(FEditorNotificationManager& Notifications, Editor::FTransactionManager& Transactions) -> void;

	private:
		static auto DrawHistory(FEditorNotificationManager& Notifications, bool* bOpen) -> void;
		static auto PublishTransactionEvents(FEditorNotificationManager& Notifications, Editor::FTransactionManager& Transactions) -> void;
	};
}
