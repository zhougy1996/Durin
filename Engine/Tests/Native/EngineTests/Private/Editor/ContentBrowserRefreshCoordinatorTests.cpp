#include <gtest/gtest.h>
#include "Editor/EditorTransactionTestSupport.h"
#include "Editor/EditorTransactionTestSupport.h"

#include "Editor/Transaction.h"
#include "Panels/ContentBrowserRefreshCoordinator.h"

using namespace Durin::Editor::ContentBrowser::Private;

namespace
{
	class FRefreshTestTransaction final : public Durin::Editor::ITransactionCustomChange
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
	Durin::Tests::FTestTransactorOwner Transactions;
	const uint64 InitialRevision =
		Transactions->GetMountedContentMutationRevision();
	ASSERT_TRUE(Transactions->Execute(
		std::make_unique<FRefreshTestTransaction>(false)));
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(), InitialRevision);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(), InitialRevision);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(), InitialRevision);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	MountedContentTransactionsPublishEverySuccessfulTransition)
{
	Durin::Tests::FTestTransactorOwner Transactions;
	const uint64 InitialRevision =
		Transactions->GetMountedContentMutationRevision();
	ASSERT_TRUE(Transactions->Execute(
		std::make_unique<FRefreshTestTransaction>(true)));
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(), InitialRevision + 1);
	ASSERT_TRUE(Transactions->Undo());
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(), InitialRevision + 2);
	ASSERT_TRUE(Transactions->Redo());
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(), InitialRevision + 3);
	Transactions->NotifyMountedContentMutation();
	EXPECT_EQ(
		Transactions->GetMountedContentMutationRevision(), InitialRevision + 4);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	RegistryOnlyPublicationRefreshesWithoutScanning)
{
	FContentBrowserRefreshCoordinator Coordinator(4, 10);
	int ScanCount = 0;
	int RefreshCount = 0;
	uint64 RegistryRevision = 11;
	const auto Reconcile = [&] {
		++ScanCount;
		return Durin::FAssetResult{};
	};
	const auto Refresh = [&](const Durin::FContentChangeBatch&) { ++RefreshCount; };
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
	FContentBrowserRefreshCoordinator Coordinator(7, 20);
	int ScanCount = 0;
	int RefreshCount = 0;
	uint64 RegistryRevision = 20;
	const auto Reconcile = [&] {
		++ScanCount;
		++RegistryRevision;
		return Durin::FAssetResult{};
	};
	const auto Refresh = [&](const Durin::FContentChangeBatch&) { ++RefreshCount; };
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
		std::make_shared<FMountedContentReconciliationState>();
	FContentBrowserRefreshCoordinator First(5, 12, SharedState);
	FContentBrowserRefreshCoordinator Second(5, 12, SharedState);
	int ScanCount = 0;
	int FirstRefreshCount = 0;
	int SecondRefreshCount = 0;
	uint64 RegistryRevision = 12;
	const auto Reconcile = [&] {
		++ScanCount;
		++RegistryRevision;
		return Durin::FAssetResult{};
	};
	const auto GetRegistryRevision = [&] { return RegistryRevision; };

	ASSERT_TRUE(First.Synchronize(
		6, RegistryRevision, Reconcile,
		[&](const Durin::FContentChangeBatch&) { ++FirstRefreshCount; }, GetRegistryRevision));
	ASSERT_TRUE(Second.Synchronize(
		6, RegistryRevision, Reconcile,
		[&](const Durin::FContentChangeBatch&) { ++SecondRefreshCount; }, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 1);
	EXPECT_EQ(FirstRefreshCount, 1);
	EXPECT_EQ(SecondRefreshCount, 1);
}

TEST(FContentBrowserRefreshCoordinatorTests,
	FailedRevisionDoesNotSpinAndExplicitRefreshRetriesPendingWork)
{
	FContentBrowserRefreshCoordinator Coordinator(2, 30);
	int ScanCount = 0;
	int RefreshCount = 0;
	uint64 RegistryRevision = 30;
	bool bFail = true;
	const auto Reconcile = [&] {
		++ScanCount;
		if (bFail)
			return Durin::FAssetResult{
				Durin::EAssetError::IoError, "forced scan failure"};
		++RegistryRevision;
		return Durin::FAssetResult{};
	};
	const auto Refresh = [&](const Durin::FContentChangeBatch&) { ++RefreshCount; };
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
	FContentBrowserRefreshCoordinator Coordinator(8, 40);
	int ScanCount = 0;
	int RefreshCount = 0;
	bool bFail = true;
	const auto Reconcile = [&] {
		++ScanCount;
		if (bFail)
			return Durin::FAssetResult{
				Durin::EAssetError::IoError, "forced scan failure"};
		return Durin::FAssetResult{};
	};
	const auto Refresh = [&](const Durin::FContentChangeBatch&) { ++RefreshCount; };
	const auto GetRegistryRevision = [] { return uint64{40}; };

	EXPECT_FALSE(Coordinator.Synchronize(
		9, 40, Reconcile, Refresh, GetRegistryRevision));
	bFail = false;
	ASSERT_TRUE(Coordinator.Synchronize(
		10, 40, Reconcile, Refresh, GetRegistryRevision));
	EXPECT_EQ(ScanCount, 2);
	EXPECT_EQ(RefreshCount, 1);
	EXPECT_EQ(Coordinator.GetObservedMountedContentRevision(), 10);
}

TEST(FContentBrowserRefreshCoordinatorTests, ChangeJournalReadersAreIndependentAndGapsAreExplicit)
{
	using namespace Durin;
	FContentChangeJournal Journal(2, 2);
	Journal.Append({1, 2, false, {{EContentChangeKind::Added, {}, "/A/file"}}});
	EXPECT_EQ(Journal.Read(1, 2).Changes.size(), 1u);
	EXPECT_EQ(Journal.Read(1, 2).Changes.size(), 1u);
	Journal.Append({2, 3, false, {{EContentChangeKind::Renamed, "/A/file", "/B/file"}}});
	EXPECT_EQ(Journal.Read(1, 3).Changes.size(), 2u);
	Journal.Append({3, 4, false, {}});
	EXPECT_TRUE(Journal.Read(1, 4).bFullRefresh);
	EXPECT_FALSE(Journal.Read(2, 4).bFullRefresh);
	Journal.Append({4, 5, false, {{}, {}, {}}});
	EXPECT_TRUE(Journal.Read(4, 5).bFullRefresh);
	EXPECT_FALSE(Journal.Read(5, 5).bFullRefresh);
}

TEST(FContentBrowserRefreshCoordinatorTests, ScopedNotificationsRetainOrderedMappingsAndLegacyFallback)
{
	using namespace Durin;
	Tests::FTestTransactorOwner Transactions;
	const uint64 Start = Transactions->GetMountedContentMutationRevision();
	Transactions->NotifyMountedContentMutation({.Changes = {
		{EContentChangeKind::Renamed, "/A", "/B", {}, {}, true}}});
	Transactions->NotifyMountedContentMutation({.Changes = {
		{EContentChangeKind::Renamed, "/B", "/C", {}, {}, true}}});
	const auto First = Transactions->CaptureMountedContentChanges(Start);
	const auto Second = Transactions->CaptureMountedContentChanges(Start);
	ASSERT_FALSE(First.bFullRefresh);
	ASSERT_EQ(First.Changes.size(), 2u);
	EXPECT_EQ(First.Changes[0].OldPhysicalPath, "/A");
	EXPECT_EQ(First.Changes[1].NewPhysicalPath, "/C");
	EXPECT_EQ(Second.Changes.size(), First.Changes.size());
	Transactions->NotifyMountedContentMutation();
	EXPECT_TRUE(Transactions->CaptureMountedContentChanges(Start).bFullRefresh);
}

TEST(FContentBrowserRefreshCoordinatorTests, SharedReconciliationKeepsIndependentScopedCursors)
{
	using namespace Durin;
	auto Shared = std::make_shared<FMountedContentReconciliationState>();
	FContentBrowserRefreshCoordinator First(1, 1, Shared), Second(1, 1, Shared);
	FContentChangeJournal Mounted;
	uint64 Revision = 2;
	Mounted.Append({1, 2, false, {{EContentChangeKind::Renamed, "/A", "/B", {}, {}, true}}});
	const auto Capture = [&](uint64 From) { return Mounted.Read(From, Revision); };
	First.SetChangeSources(Capture, {});
	Second.SetChangeSources(Capture, {});
	int Scans = 0;
	const auto Reconcile = [&] { ++Scans; return FAssetResult{}; };
	FContentChangeBatch FirstChanges, SecondChanges;
	ASSERT_TRUE(First.Synchronize(Revision, 1, Reconcile,
		[&](const auto& Batch) { FirstChanges = Batch; }, [] { return uint64{1}; }));
	Mounted.Append({2, 3, false, {{EContentChangeKind::Renamed, "/B", "/C", {}, {}, true}}});
	Revision = 3;
	ASSERT_TRUE(First.Synchronize(Revision, 1, Reconcile,
		[&](const auto& Batch) { FirstChanges = Batch; }, [] { return uint64{1}; }));
	ASSERT_TRUE(Second.Synchronize(Revision, 1, Reconcile,
		[&](const auto& Batch) { SecondChanges = Batch; }, [] { return uint64{1}; }));
	EXPECT_EQ(Scans, 2);
	EXPECT_FALSE(FirstChanges.bFullRefresh);
	EXPECT_FALSE(SecondChanges.bFullRefresh);
	EXPECT_EQ(FirstChanges.Changes.size(), 1u);
	EXPECT_EQ(SecondChanges.Changes.size(), 2u);
}
