#pragma once

#include "DurinEdAPI.h"

namespace Durin::Editor
{
	using FNotificationId = uint64;

	// Selects notification styling and default lifetime behavior.
	enum class ENotificationType : uint8
	{
		Info,
		Success,
		Warning,
		Error,
		Progress,
	};

	// Selects where a notification is surfaced while every entry remains available in session history.
	enum class ENotificationPresentation : uint8
	{
		Toast,
		StatusBar,
		HistoryOnly,
	};

	// Defines an optional user action attached to a notification.
	struct FNotificationAction
	{
		std::string Label;
		std::function<bool()> IsEnabled;
		std::function<void()> Invoke;
	};

	// Describes a transient notification submitted by any producer thread.
	struct FNotificationDesc
	{
		ENotificationType Type = ENotificationType::Info;
		std::string Message;
		// Optional session-history context; toast notifications intentionally remain concise.
		std::string Details;
		// Negative values select the type default, zero keeps the notification until dismissed.
		float DurationSeconds = -1.0f;
		std::optional<FNotificationAction> Action;
		ENotificationPresentation Presentation = ENotificationPresentation::Toast;
	};

	// Describes a cancellable progress notification with optional progress.
	struct FProgressNotificationDesc
	{
		std::string Message;
		std::optional<float> Progress;
		std::optional<FNotificationAction> Action;
		std::function<void()> Cancel;
		ENotificationPresentation Presentation = ENotificationPresentation::Toast;
	};

	// Stores game-thread notification state shared by overlays and history.
	struct FNotification
	{
		FNotificationId Id = 0;
		ENotificationType Type = ENotificationType::Info;
		std::string Message;
		std::string Details;
		std::optional<float> Progress;
		std::optional<float> RemainingSeconds;
		std::optional<FNotificationAction> Action;
		std::function<void()> Cancel;
		bool bCancelRequested = false;
		bool bHovered = false;
	};

	// Serializes notification producers and owns live and session-history entries.
	class FNotificationManager
	{
	public:
		DURINED_API auto Post(FNotificationDesc Desc) -> FNotificationId;
		DURINED_API auto BeginProgress(FProgressNotificationDesc Desc) -> FNotificationId;
		DURINED_API auto UpdateProgress(FNotificationId Id, std::optional<float> Progress, std::string Message = {}) -> void;
		DURINED_API auto CompleteProgress(FNotificationId Id, std::string Message = {}) -> void;
		DURINED_API auto FailProgress(FNotificationId Id, std::string Message) -> void;
		DURINED_API auto Dismiss(FNotificationId Id) -> void;

		// Tick and interaction methods are game-thread owned. Producer methods above only enqueue commands.
		DURINED_API auto Tick(float DeltaSeconds) -> void;
		DURINED_API auto SetHovered(FNotificationId Id, bool bHovered) -> void;
		DURINED_API auto InvokeAction(FNotificationId Id) -> bool;
		DURINED_API auto RequestCancel(FNotificationId Id) -> bool;
		auto GetNotifications() const -> const std::vector<FNotification>& { return Notifications; }
		auto GetStatusNotification() const -> const std::optional<FNotification>& { return StatusNotification; }
		auto GetHistory() const -> const std::vector<FNotification>& { return History; }
		DURINED_API auto ClearHistory() -> void;

	private:
		auto Enqueue(std::function<void()> Command) -> void;
		auto Find(FNotificationId Id) -> FNotification*;
		auto FindStatus(FNotificationId Id) -> FNotification*;
		auto FindHistory(FNotificationId Id) -> FNotification*;
		auto UpdateAll(FNotificationId Id, const std::function<void(FNotification&)>& Update) -> void;
		static auto ResolveDuration(ENotificationType Type, float RequestedSeconds) -> std::optional<float>;

		std::atomic<FNotificationId> NextId = 1;
		std::mutex PendingMutex;
		std::vector<std::function<void()>> PendingCommands;
		std::vector<FNotification> Notifications;
		std::optional<FNotification> StatusNotification;
		// History is session-only and intentionally independent from toast lifetime/dismissal.
		std::vector<FNotification> History;
	};
}
