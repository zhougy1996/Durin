#pragma once

namespace Durin
{
	class FEditorNotificationManager;
	class FEditorTransactionManager;

	class FEditorNotificationOverlay
	{
	public:
		auto Draw(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions, bool* bHistoryOpen) -> void;

	private:
		static auto DrawHistory(FEditorNotificationManager& Notifications, bool* bOpen) -> void;
		static auto PublishTransactionEvents(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions) -> void;
	};
}
