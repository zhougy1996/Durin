#include "Editor/EditorNotification.h"

namespace Durin
{
	namespace
	{
		constexpr float DefaultInfoDuration = 5.0f;
		constexpr float DefaultWarningDuration = 8.0f;
	}

	auto FEditorNotificationManager::Post(FEditorNotificationDesc Desc) -> FEditorNotificationId
	{
		const FEditorNotificationId Id = NextId.fetch_add(1, std::memory_order_relaxed);
		Enqueue([this, Id, Desc = std::move(Desc)]() mutable {
			FEditorNotification Notification;
			Notification.Id = Id;
			Notification.Type = Desc.Type;
			Notification.Message = std::move(Desc.Message);
			Notification.RemainingSeconds = ResolveDuration(Notification.Type, Desc.DurationSeconds);
			Notification.Action = std::move(Desc.Action);
			Notifications.emplace_back(std::move(Notification));
		});
		return Id;
	}

	auto FEditorNotificationManager::BeginProgress(FEditorProgressNotificationDesc Desc) -> FEditorNotificationId
	{
		const FEditorNotificationId Id = NextId.fetch_add(1, std::memory_order_relaxed);
		Enqueue([this, Id, Desc = std::move(Desc)]() mutable {
			FEditorNotification Notification;
			Notification.Id = Id;
			Notification.Type = EEditorNotificationType::Progress;
			Notification.Message = std::move(Desc.Message);
			if (Desc.Progress) Notification.Progress = std::clamp(*Desc.Progress, 0.0f, 1.0f);
			Notification.Cancel = std::move(Desc.Cancel);
			Notifications.emplace_back(std::move(Notification));
		});
		return Id;
	}

	auto FEditorNotificationManager::UpdateProgress(FEditorNotificationId Id, std::optional<float> Progress, std::string Message) -> void
	{
		Enqueue([this, Id, Progress, Message = std::move(Message)]() mutable {
			FEditorNotification* Notification = Find(Id);
			if (!Notification || Notification->Type != EEditorNotificationType::Progress) return;
			Notification->Progress = Progress ? std::optional(std::clamp(*Progress, 0.0f, 1.0f)) : std::nullopt;
			if (!Message.empty()) Notification->Message = std::move(Message);
		});
	}

	auto FEditorNotificationManager::CompleteProgress(FEditorNotificationId Id, std::string Message) -> void
	{
		Enqueue([this, Id, Message = std::move(Message)]() mutable {
			FEditorNotification* Notification = Find(Id);
			if (!Notification || Notification->Type != EEditorNotificationType::Progress) return;
			Notification->Type = EEditorNotificationType::Success;
			Notification->Progress.reset();
			Notification->RemainingSeconds = DefaultInfoDuration;
			Notification->Cancel = {};
			if (!Message.empty()) Notification->Message = std::move(Message);
		});
	}

	auto FEditorNotificationManager::FailProgress(FEditorNotificationId Id, std::string Message) -> void
	{
		Enqueue([this, Id, Message = std::move(Message)]() mutable {
			FEditorNotification* Notification = Find(Id);
			if (!Notification || Notification->Type != EEditorNotificationType::Progress) return;
			Notification->Type = EEditorNotificationType::Error;
			Notification->Progress.reset();
			Notification->RemainingSeconds.reset();
			Notification->Cancel = {};
			Notification->Message = std::move(Message);
		});
	}

	auto FEditorNotificationManager::Dismiss(FEditorNotificationId Id) -> void
	{
		Enqueue([this, Id] {
			std::erase_if(Notifications, [Id](const FEditorNotification& Notification) { return Notification.Id == Id; });
		});
	}

	auto FEditorNotificationManager::Tick(float DeltaSeconds) -> void
	{
		std::vector<std::function<void()>> Commands;
		{
			std::scoped_lock Lock(PendingMutex);
			Commands.swap(PendingCommands);
		}
		for (std::function<void()>& Command : Commands) Command();

		const float SafeDelta = std::max(DeltaSeconds, 0.0f);
		for (FEditorNotification& Notification : Notifications)
		{
			if (Notification.RemainingSeconds && !Notification.bHovered)
			{
				*Notification.RemainingSeconds -= SafeDelta;
			}
			Notification.bHovered = false;
		}
		std::erase_if(Notifications, [](const FEditorNotification& Notification) {
			return Notification.RemainingSeconds && *Notification.RemainingSeconds <= 0.0f;
		});
	}

	auto FEditorNotificationManager::SetHovered(FEditorNotificationId Id, bool bHovered) -> void
	{
		if (FEditorNotification* Notification = Find(Id)) Notification->bHovered = bHovered;
	}

	auto FEditorNotificationManager::InvokeAction(FEditorNotificationId Id) -> bool
	{
		FEditorNotification* Notification = Find(Id);
		if (!Notification || !Notification->Action || !Notification->Action->Invoke) return false;
		if (Notification->Action->IsEnabled && !Notification->Action->IsEnabled()) return false;
		const std::function<void()> Callback = Notification->Action->Invoke;
		Callback();
		return true;
	}

	auto FEditorNotificationManager::RequestCancel(FEditorNotificationId Id) -> bool
	{
		FEditorNotification* Notification = Find(Id);
		if (!Notification || Notification->Type != EEditorNotificationType::Progress || !Notification->Cancel || Notification->bCancelRequested) return false;
		Notification->bCancelRequested = true;
		const std::function<void()> Callback = Notification->Cancel;
		Callback();
		return true;
	}

	auto FEditorNotificationManager::Enqueue(std::function<void()> Command) -> void
	{
		std::scoped_lock Lock(PendingMutex);
		PendingCommands.emplace_back(std::move(Command));
	}

	auto FEditorNotificationManager::Find(FEditorNotificationId Id) -> FEditorNotification*
	{
		const auto Iterator = std::ranges::find(Notifications, Id, &FEditorNotification::Id);
		return Iterator == Notifications.end() ? nullptr : &*Iterator;
	}

	auto FEditorNotificationManager::ResolveDuration(EEditorNotificationType Type, float RequestedSeconds) -> std::optional<float>
	{
		if (RequestedSeconds == 0.0f || Type == EEditorNotificationType::Error || Type == EEditorNotificationType::Progress) return std::nullopt;
		if (RequestedSeconds > 0.0f) return RequestedSeconds;
		return Type == EEditorNotificationType::Warning ? DefaultWarningDuration : DefaultInfoDuration;
	}
}
