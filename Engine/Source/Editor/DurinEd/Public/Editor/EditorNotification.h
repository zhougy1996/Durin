#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	using FEditorNotificationId = uint64;

	enum class EEditorNotificationType : uint8
	{
		Info,
		Success,
		Warning,
		Error,
		Progress,
	};

	struct FEditorNotificationAction
	{
		std::string Label;
		std::function<bool()> IsEnabled;
		std::function<void()> Invoke;
	};

	struct FEditorNotificationDesc
	{
		EEditorNotificationType Type = EEditorNotificationType::Info;
		std::string Message;
		// Negative values select the type default, zero keeps the notification until dismissed.
		float DurationSeconds = -1.0f;
		std::optional<FEditorNotificationAction> Action;
	};

	struct FEditorProgressNotificationDesc
	{
		std::string Message;
		std::optional<float> Progress;
		std::function<void()> Cancel;
	};

	struct FEditorNotification
	{
		FEditorNotificationId Id = 0;
		EEditorNotificationType Type = EEditorNotificationType::Info;
		std::string Message;
		std::optional<float> Progress;
		std::optional<float> RemainingSeconds;
		std::optional<FEditorNotificationAction> Action;
		std::function<void()> Cancel;
		bool bCancelRequested = false;
		bool bHovered = false;
	};

	class FEditorNotificationManager
	{
	public:
		DURINED_API auto Post(FEditorNotificationDesc Desc) -> FEditorNotificationId;
		DURINED_API auto BeginProgress(FEditorProgressNotificationDesc Desc) -> FEditorNotificationId;
		DURINED_API auto UpdateProgress(FEditorNotificationId Id, std::optional<float> Progress, std::string Message = {}) -> void;
		DURINED_API auto CompleteProgress(FEditorNotificationId Id, std::string Message = {}) -> void;
		DURINED_API auto FailProgress(FEditorNotificationId Id, std::string Message) -> void;
		DURINED_API auto Dismiss(FEditorNotificationId Id) -> void;

		// Tick and interaction methods are game-thread owned. Producer methods above only enqueue commands.
		DURINED_API auto Tick(float DeltaSeconds) -> void;
		DURINED_API auto SetHovered(FEditorNotificationId Id, bool bHovered) -> void;
		DURINED_API auto InvokeAction(FEditorNotificationId Id) -> bool;
		DURINED_API auto RequestCancel(FEditorNotificationId Id) -> bool;
		auto GetNotifications() const -> const std::vector<FEditorNotification>& { return Notifications; }
		auto GetHistory() const -> const std::vector<FEditorNotification>& { return History; }
		DURINED_API auto ClearHistory() -> void;

	private:
		auto Enqueue(std::function<void()> Command) -> void;
		auto Find(FEditorNotificationId Id) -> FEditorNotification*;
		auto FindHistory(FEditorNotificationId Id) -> FEditorNotification*;
		auto UpdateBoth(FEditorNotificationId Id, const std::function<void(FEditorNotification&)>& Update) -> void;
		static auto ResolveDuration(EEditorNotificationType Type, float RequestedSeconds) -> std::optional<float>;

		std::atomic<FEditorNotificationId> NextId = 1;
		std::mutex PendingMutex;
		std::vector<std::function<void()>> PendingCommands;
		std::vector<FEditorNotification> Notifications;
		// History is session-only and intentionally independent from toast lifetime/dismissal.
		std::vector<FEditorNotification> History;
	};
}
