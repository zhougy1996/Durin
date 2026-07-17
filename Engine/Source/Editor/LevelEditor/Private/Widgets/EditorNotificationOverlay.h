#pragma once

#include "Panels/LevelEditorPanel.h"

namespace Durin
{
	class FEditorNotificationManager;
	class FEditorTransactionManager;

	class FEditorNotificationOverlay final : public ILevelEditorPanel
	{
	public:
		auto GetWindowName() const -> const char* override { return "Activity History"; }
		auto Draw(FLevelEditorContext& Context) -> void override;
		auto DrawNotifications(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions) -> void;

	private:
		static auto DrawHistory(FEditorNotificationManager& Notifications, bool* bOpen) -> void;
		static auto PublishTransactionEvents(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions) -> void;
	};
}
