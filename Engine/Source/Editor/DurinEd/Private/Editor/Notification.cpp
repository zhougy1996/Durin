#include "Editor/Notification.h"

namespace Durin::Editor
{
	namespace
	{
		constexpr float DefaultInfoDuration = 5.0f;
		constexpr float DefaultWarningDuration = 8.0f;
	}

	auto FNotificationManager::Post(FNotificationDesc Desc) -> FNotificationId
	{
		const FNotificationId Id = NextId.fetch_add(1, std::memory_order_relaxed);
		Enqueue([this, Id, Desc = std::move(Desc)]() mutable {
			FNotification Notification;
			Notification.Id = Id;
			Notification.Type = Desc.Type;
			Notification.Message = std::move(Desc.Message);
			Notification.Details = std::move(Desc.Details);
			Notification.RemainingSeconds = ResolveDuration(Notification.Type, Desc.DurationSeconds);
			Notification.Action = std::move(Desc.Action);
			History.emplace_back(Notification);
			Notifications.emplace_back(std::move(Notification));
		});
		return Id;
	}

	auto FNotificationManager::BeginProgress(FProgressNotificationDesc Desc) -> FNotificationId
	{
		const FNotificationId Id = NextId.fetch_add(1, std::memory_order_relaxed);
		Enqueue([this, Id, Desc = std::move(Desc)]() mutable {
			FNotification Notification;
			Notification.Id = Id;
			Notification.Type = ENotificationType::Progress;
			Notification.Message = std::move(Desc.Message);
			if (Desc.Progress) Notification.Progress = std::clamp(*Desc.Progress, 0.0f, 1.0f);
			Notification.Action = std::move(Desc.Action);
			Notification.Cancel = std::move(Desc.Cancel);
			History.emplace_back(Notification);
			Notifications.emplace_back(std::move(Notification));
		});
		return Id;
	}

	auto FNotificationManager::UpdateProgress(FNotificationId Id, std::optional<float> Progress, std::string Message) -> void
	{
		Enqueue([this, Id, Progress, Message = std::move(Message)]() mutable {
			UpdateBoth(Id, [&](FNotification& Notification) {
				if (Notification.Type != ENotificationType::Progress) return;
				Notification.Progress = Progress ? std::optional(std::clamp(*Progress, 0.0f, 1.0f)) : std::nullopt;
				if (!Message.empty()) Notification.Message = Message;
			});
		});
	}

	auto FNotificationManager::CompleteProgress(FNotificationId Id, std::string Message) -> void
	{
		Enqueue([this, Id, Message = std::move(Message)]() mutable {
			UpdateBoth(Id, [&](FNotification& Notification) {
				if (Notification.Type != ENotificationType::Progress) return;
				Notification.Type = ENotificationType::Success;
				Notification.Progress.reset();
				Notification.RemainingSeconds = DefaultInfoDuration;
				Notification.Cancel = {};
				if (!Message.empty()) Notification.Message = Message;
			});
		});
	}

	auto FNotificationManager::FailProgress(FNotificationId Id, std::string Message) -> void
	{
		Enqueue([this, Id, Message = std::move(Message)]() mutable {
			UpdateBoth(Id, [&](FNotification& Notification) {
				if (Notification.Type != ENotificationType::Progress) return;
				Notification.Type = ENotificationType::Error;
				Notification.Progress.reset();
				Notification.RemainingSeconds.reset();
				Notification.Cancel = {};
				Notification.Message = Message;
			});
		});
	}

	auto FNotificationManager::Dismiss(FNotificationId Id) -> void
	{
		Enqueue([this, Id] {
			std::erase_if(Notifications, [Id](const FNotification& Notification) { return Notification.Id == Id; });
		});
	}

	auto FNotificationManager::Tick(float DeltaSeconds) -> void
	{
		std::vector<std::function<void()>> Commands;
		{
			std::scoped_lock Lock(PendingMutex);
			Commands.swap(PendingCommands);
		}
		for (std::function<void()>& Command : Commands) Command();

		const float SafeDelta = std::max(DeltaSeconds, 0.0f);
		for (FNotification& Notification : Notifications)
		{
			if (Notification.RemainingSeconds && !Notification.bHovered)
			{
				*Notification.RemainingSeconds -= SafeDelta;
			}
			Notification.bHovered = false;
		}
		std::erase_if(Notifications, [](const FNotification& Notification) {
			return Notification.RemainingSeconds && *Notification.RemainingSeconds <= 0.0f;
		});
	}

	auto FNotificationManager::SetHovered(FNotificationId Id, bool bHovered) -> void
	{
		if (FNotification* Notification = Find(Id)) Notification->bHovered = bHovered;
	}

	auto FNotificationManager::InvokeAction(FNotificationId Id) -> bool
	{
		FNotification* Notification = Find(Id);
		if (!Notification) Notification = FindHistory(Id);
		if (!Notification || !Notification->Action || !Notification->Action->Invoke) return false;
		if (Notification->Action->IsEnabled && !Notification->Action->IsEnabled()) return false;
		const std::function<void()> Callback = Notification->Action->Invoke;
		Callback();
		return true;
	}

	auto FNotificationManager::RequestCancel(FNotificationId Id) -> bool
	{
		FNotification* Notification = Find(Id);
		if (!Notification) Notification = FindHistory(Id);
		if (!Notification || Notification->Type != ENotificationType::Progress || !Notification->Cancel || Notification->bCancelRequested) return false;
		const std::function<void()> Callback = Notification->Cancel;
		UpdateBoth(Id, [](FNotification& Item) { Item.bCancelRequested = true; });
		Callback();
		return true;
	}

	auto FNotificationManager::ClearHistory() -> void
	{
		History.clear();
	}

	auto FNotificationManager::Enqueue(std::function<void()> Command) -> void
	{
		std::scoped_lock Lock(PendingMutex);
		PendingCommands.emplace_back(std::move(Command));
	}

	auto FNotificationManager::Find(FNotificationId Id) -> FNotification*
	{
		const auto Iterator = std::ranges::find(Notifications, Id, &FNotification::Id);
		return Iterator == Notifications.end() ? nullptr : &*Iterator;
	}

	auto FNotificationManager::FindHistory(FNotificationId Id) -> FNotification*
	{
		const auto Iterator = std::ranges::find(History, Id, &FNotification::Id);
		return Iterator == History.end() ? nullptr : &*Iterator;
	}

	auto FNotificationManager::UpdateBoth(FNotificationId Id, const std::function<void(FNotification&)>& Update) -> void
	{
		if (FNotification* Notification = Find(Id)) Update(*Notification);
		if (FNotification* Notification = FindHistory(Id)) Update(*Notification);
	}

	auto FNotificationManager::ResolveDuration(ENotificationType Type, float RequestedSeconds) -> std::optional<float>
	{
		if (RequestedSeconds == 0.0f || Type == ENotificationType::Error || Type == ENotificationType::Progress) return std::nullopt;
		if (RequestedSeconds > 0.0f) return RequestedSeconds;
		return Type == ENotificationType::Warning ? DefaultWarningDuration : DefaultInfoDuration;
	}
}
