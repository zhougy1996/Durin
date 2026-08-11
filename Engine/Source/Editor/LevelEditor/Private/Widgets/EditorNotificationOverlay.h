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
		auto DrawNotifications(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void;

	private:
		static auto DrawHistory(::Durin::Editor::FNotificationManager& Notifications, bool* bOpen) -> void;
		static auto PublishTransactionEvents(::Durin::Editor::FNotificationManager& Notifications, ::Durin::Editor::FTransactionManager& Transactions) -> void;
	};
}
