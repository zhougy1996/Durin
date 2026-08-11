#include "Editor/Notification.h"

#include <gtest/gtest.h>

namespace
{
	const Durin::Editor::FNotification* FindNotification(const Durin::Editor::FNotificationManager& Manager, Durin::Editor::FNotificationId Id)
	{
		const auto& Notifications = Manager.GetNotifications();
		const auto Iterator = std::ranges::find(Notifications, Id, &Durin::Editor::FNotification::Id);
		return Iterator == Notifications.end() ? nullptr : &*Iterator;
	}
}

TEST(FNotificationManagerTests, AppliesDefaultLifetimeAndPausesWhileHovered)
{
	Durin::Editor::FNotificationManager Manager;
	const Durin::Editor::FNotificationId Id = Manager.Post({
		.Type = Durin::Editor::ENotificationType::Success,
		.Message = "Saved",
		.Details = "Package '/Game/Example'",
	});
	Manager.Tick(0.0f);
	ASSERT_NE(FindNotification(Manager, Id), nullptr);

	Manager.SetHovered(Id, true);
	Manager.Tick(10.0f);
	ASSERT_NE(FindNotification(Manager, Id), nullptr);

	Manager.Tick(5.1f);
	EXPECT_EQ(FindNotification(Manager, Id), nullptr);
	ASSERT_EQ(Manager.GetHistory().size(), 1);
	EXPECT_EQ(Manager.GetHistory().front().Message, "Saved");
	EXPECT_EQ(Manager.GetHistory().front().Details, "Package '/Game/Example'");
}

TEST(FNotificationManagerTests, UpdatesCompletesAndFailsProgressNotifications)
{
	Durin::Editor::FNotificationManager Manager;
	const Durin::Editor::FNotificationId CompletedId = Manager.BeginProgress({.Message = "Importing"});
	const Durin::Editor::FNotificationId FailedId = Manager.BeginProgress({.Message = "Cooking", .Progress = 0.25f});
	Manager.UpdateProgress(CompletedId, 1.5f, "Finalizing");
	Manager.CompleteProgress(CompletedId, "Imported");
	Manager.FailProgress(FailedId, "Cook failed");
	Manager.Tick(0.0f);

	const Durin::Editor::FNotification* Completed = FindNotification(Manager, CompletedId);
	ASSERT_NE(Completed, nullptr);
	EXPECT_EQ(Completed->Type, Durin::Editor::ENotificationType::Success);
	EXPECT_EQ(Completed->Message, "Imported");
	EXPECT_FALSE(Completed->Progress.has_value());
	EXPECT_TRUE(Completed->RemainingSeconds.has_value());

	const Durin::Editor::FNotification* Failed = FindNotification(Manager, FailedId);
	ASSERT_NE(Failed, nullptr);
	EXPECT_EQ(Failed->Type, Durin::Editor::ENotificationType::Error);
	EXPECT_EQ(Failed->Message, "Cook failed");
	EXPECT_FALSE(Failed->RemainingSeconds.has_value());
}

TEST(FNotificationManagerTests, RequestsCancellationOnlyOnce)
{
	Durin::Editor::FNotificationManager Manager;
	int CancelCount = 0;
	const Durin::Editor::FNotificationId Id = Manager.BeginProgress({
		.Message = "Loading",
		.Cancel = [&CancelCount] { ++CancelCount; },
	});
	Manager.Tick(0.0f);

	EXPECT_TRUE(Manager.RequestCancel(Id));
	EXPECT_FALSE(Manager.RequestCancel(Id));
	EXPECT_EQ(CancelCount, 1);
	const Durin::Editor::FNotification* Notification = FindNotification(Manager, Id);
	ASSERT_NE(Notification, nullptr);
	EXPECT_TRUE(Notification->bCancelRequested);
}

TEST(FNotificationManagerTests, AcceptsCommandsFromAnotherThread)
{
	Durin::Editor::FNotificationManager Manager;
	Durin::Editor::FNotificationId Id = 0;
	std::thread Producer([&] {
		Id = Manager.BeginProgress({.Message = "Background"});
		Manager.UpdateProgress(Id, 0.5f, "Halfway");
	});
	Producer.join();
	Manager.Tick(0.0f);

	const Durin::Editor::FNotification* Notification = FindNotification(Manager, Id);
	ASSERT_NE(Notification, nullptr);
	ASSERT_TRUE(Notification->Progress.has_value());
	EXPECT_FLOAT_EQ(*Notification->Progress, 0.5f);
	EXPECT_EQ(Notification->Message, "Halfway");
	ASSERT_EQ(Manager.GetHistory().size(), 1);
	EXPECT_EQ(Manager.GetHistory().front().Message, "Halfway");
}

TEST(FNotificationManagerTests, DismissalPreservesHistoryUntilExplicitlyCleared)
{
	Durin::Editor::FNotificationManager Manager;
	const Durin::Editor::FNotificationId Id = Manager.Post({.Message = "Moved Actor"});
	Manager.Tick(0.0f);
	Manager.Dismiss(Id);
	Manager.Tick(0.0f);

	EXPECT_EQ(FindNotification(Manager, Id), nullptr);
	ASSERT_EQ(Manager.GetHistory().size(), 1);
	EXPECT_EQ(Manager.GetHistory().front().Id, Id);

	Manager.ClearHistory();
	EXPECT_TRUE(Manager.GetHistory().empty());
}
