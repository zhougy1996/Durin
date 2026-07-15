#pragma once

namespace Durin
{
	class FEditorNotificationManager;
	class FEditorTransactionManager;

	class FEditorNotificationOverlay
	{
	public:
		auto Draw(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions) -> void;

	private:
		static auto PublishTransactionEvents(FEditorNotificationManager& Notifications, FEditorTransactionManager& Transactions) -> void;
	};
}
