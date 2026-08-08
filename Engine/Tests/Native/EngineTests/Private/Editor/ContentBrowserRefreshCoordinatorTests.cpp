#include <gtest/gtest.h>

#include "Editor/EditorTransaction.h"
#include "Panels/ContentBrowserRefreshCoordinator.h"

namespace
{
	class FRefreshTestTransaction final : public Durin::IEditorTransaction
	{
	public:
		explicit FRefreshTestTransaction(bool bInMutatesMountedContent)
			: bMutatesMountedContent(bInMutatesMountedContent)
		{
		}

		auto GetDescription() const -> std::string_view override
		{
			return "Refresh test transaction";
		}
		auto MutatesMountedContent() const -> bool override
		{
			return bMutatesMountedContent;
		}
		auto Undo() -> bool override { return true; }
		auto Redo() -> bool override { return true; }

	private:
		bool bMutatesMountedContent = false;
	};
}

TEST(FContentBrowserRefreshCoordinatorTests,
	OrdinaryTransactionsNeverPublishMountedContentInvalidation)
{
	Durin::FEditorTransactionManager Transactions;
	const Durin::uint64 InitialRevision =
		Transactions.GetMountedContentMutationRevision();
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FRefreshTestTransaction>(false)));
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialRevision);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialRevision);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialRevision);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	MountedContentTransactionsPublishEverySuccessfulTransition)
{
	Durin::FEditorTransactionManager Transactions;
	const Durin::uint64 InitialRevision =
		Transactions.GetMountedContentMutationRevision();
	ASSERT_TRUE(Transactions.Execute(
		std::make_unique<FRefreshTestTransaction>(true)));
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialRevision + 1);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialRevision + 2);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialRevision + 3);
	Transactions.NotifyMountedContentMutation();
	EXPECT_EQ(
		Transactions.GetMountedContentMutationRevision(), InitialRevision + 4);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	RegistryOnlyPublicationRefreshesWithoutScanning)
{
	Durin::FContentBrowserRefreshCoordinator Coordinator(4, 10);
	int ScanCount = 0;
	int RefreshCount = 0;
	Durin::uint64 RegistryRevision = 11;
	const auto Reconcile = [&] {
		++ScanCount;
		return Durin::Asset::FAssetResult{};
	};
	const auto Refresh = [&] { ++RefreshCount; };
	const auto GetRegistryRevision = [&] { return RegistryRevision; };

	ASSERT_TRUE(Coordinator.Synchronize(
		4, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 0);
	EXPECT_EQ(RefreshCount, 1);
	ASSERT_TRUE(Coordinator.Synchronize(
		4, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 0);
	EXPECT_EQ(RefreshCount, 1);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	MountedRevisionScansOnceAndAcknowledgesRegistryPublication)
{
	Durin::FContentBrowserRefreshCoordinator Coordinator(7, 20);
	int ScanCount = 0;
	int RefreshCount = 0;
	Durin::uint64 RegistryRevision = 20;
	const auto Reconcile = [&] {
		++ScanCount;
		++RegistryRevision;
		return Durin::Asset::FAssetResult{};
	};
	const auto Refresh = [&] { ++RefreshCount; };
	const auto GetRegistryRevision = [&] { return RegistryRevision; };

	ASSERT_TRUE(Coordinator.Synchronize(
		8, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 1);
	EXPECT_EQ(RefreshCount, 1);
	ASSERT_TRUE(Coordinator.Synchronize(
		8, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 1);
	EXPECT_EQ(RefreshCount, 1);

	ASSERT_TRUE(Coordinator.Synchronize(
		9, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 2);
	ASSERT_TRUE(Coordinator.Synchronize(
		10, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 3);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	OpenPanelsShareOneMountedContentReconciliation)
{
	auto SharedState =
		std::make_shared<Durin::FMountedContentReconciliationState>();
	Durin::FContentBrowserRefreshCoordinator First(5, 12, SharedState);
	Durin::FContentBrowserRefreshCoordinator Second(5, 12, SharedState);
	int ScanCount = 0;
	int FirstRefreshCount = 0;
	int SecondRefreshCount = 0;
	Durin::uint64 RegistryRevision = 12;
	const auto Reconcile = [&] {
		++ScanCount;
		++RegistryRevision;
		return Durin::Asset::FAssetResult{};
	};
	const auto GetRegistryRevision = [&] { return RegistryRevision; };

	ASSERT_TRUE(First.Synchronize(
		6, RegistryRevision, Reconcile,
		[&] { ++FirstRefreshCount; }, GetRegistryRevision));
	ASSERT_TRUE(Second.Synchronize(
		6, RegistryRevision, Reconcile,
		[&] { ++SecondRefreshCount; }, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 1);
	EXPECT_EQ(FirstRefreshCount, 1);
	EXPECT_EQ(SecondRefreshCount, 1);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	FailedRevisionDoesNotSpinAndExplicitRefreshRetriesPendingWork)
{
	Durin::FContentBrowserRefreshCoordinator Coordinator(2, 30);
	int ScanCount = 0;
	int RefreshCount = 0;
	Durin::uint64 RegistryRevision = 30;
	bool bFail = true;
	const auto Reconcile = [&] {
		++ScanCount;
		if (bFail)
			return Durin::Asset::FAssetResult{
				Durin::Asset::EAssetError::IoError, "forced scan failure"};
		++RegistryRevision;
		return Durin::Asset::FAssetResult{};
	};
	const auto Refresh = [&] { ++RefreshCount; };
	const auto GetRegistryRevision = [&] { return RegistryRevision; };

	EXPECT_FALSE(Coordinator.Synchronize(
		3, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 1);
	EXPECT_EQ(RefreshCount, 0);
	ASSERT_TRUE(Coordinator.Synchronize(
		3, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 1);

	RegistryRevision = 31;
	ASSERT_TRUE(Coordinator.Synchronize(
		3, RegistryRevision, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 1);
	EXPECT_EQ(RefreshCount, 1);

	bFail = false;
	ASSERT_TRUE(Coordinator.ReconcileExplicitly(
		3, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 2);
	EXPECT_EQ(RefreshCount, 2);
	EXPECT_EQ(Coordinator.GetObservedMountedContentRevision(), 3);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	LaterMountedRevisionRetriesAFailedAutomaticReconciliation)
{
	Durin::FContentBrowserRefreshCoordinator Coordinator(8, 40);
	int ScanCount = 0;
	int RefreshCount = 0;
	bool bFail = true;
	const auto Reconcile = [&] {
		++ScanCount;
		if (bFail)
			return Durin::Asset::FAssetResult{
				Durin::Asset::EAssetError::IoError, "forced scan failure"};
		return Durin::Asset::FAssetResult{};
	};
	const auto Refresh = [&] { ++RefreshCount; };
	const auto GetRegistryRevision = [] { return Durin::uint64{40}; };

	EXPECT_FALSE(Coordinator.Synchronize(
		9, 40, Reconcile, Refresh, GetRegistryRevision));
	bFail = false;
	ASSERT_TRUE(Coordinator.Synchronize(
		10, 40, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 2);
	EXPECT_EQ(RefreshCount, 1);
	EXPECT_EQ(Coordinator.GetObservedMountedContentRevision(), 10);
}
