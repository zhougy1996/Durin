#include <gtest/gtest.h>
#include "Panels/ContentBrowserRefreshCoordinator.h"
#include "Asset/LoadErrors.h"
using namespace Durin;
using namespace Durin::ContentBrowser::Private;

TEST(FContentBrowserRefreshCoordinatorTests, EventsCoalesceAcrossViewsWithoutRevisionPolling)
{
	auto State = std::make_shared<FMountedContentReconciliationState>();
	FContentBrowserRefreshCoordinator A(State), B(State);
	int Scans = 0, Refreshes = 0;
	auto Scan = [&] { ++Scans; return std::expected<void, UnifiedError::FError>{}; };
	auto Refresh = [&](const FContentChangeBatch& Batch) { ++Refreshes; EXPECT_EQ(2u, Batch.Changes.size()); };
	FContentChangeBatch Batch{.Changes = {{EContentChangeKind::Added, {}, "/Content/A"}}};
	A.ObserveMountedContent(Batch); B.ObserveMountedContent(Batch);
	A.ObserveMountedContent(Batch); B.ObserveMountedContent(Batch);
	EXPECT_TRUE(A.Synchronize(Scan, Refresh)); EXPECT_TRUE(B.Synchronize(Scan, Refresh));
	EXPECT_EQ(1, Scans); EXPECT_EQ(2, Refreshes);
	EXPECT_TRUE(A.Synchronize(Scan, Refresh)); EXPECT_EQ(1, Scans);
}
TEST(FContentBrowserRefreshCoordinatorTests, FailedReconciliationWaitsForExplicitRefreshOrAnotherEvent)
{
	FContentBrowserRefreshCoordinator Coordinator;
	int Scans = 0, Refreshes = 0;
	auto Fail = [&] { ++Scans; return std::expected<void, UnifiedError::FError>{std::unexpected(UnifiedError::FError{PackageReadErrors::IoError, "failed"})}; };
	auto Refresh = [&](const FContentChangeBatch&) { ++Refreshes; };
	Coordinator.ObserveMountedContent({.bFullRefresh = true});
	EXPECT_FALSE(Coordinator.Synchronize(Fail, Refresh));
	EXPECT_TRUE(Coordinator.Synchronize(Fail, Refresh));
	EXPECT_EQ(1, Scans); EXPECT_EQ(0, Refreshes);
	EXPECT_TRUE(Coordinator.ReconcileExplicitly([] { return std::expected<void, UnifiedError::FError>{}; }, Refresh));
	EXPECT_EQ(1, Refreshes);
	Coordinator.ObserveMountedContent({.bFullRefresh = true});
	EXPECT_FALSE(Coordinator.Synchronize(Fail, Refresh)); EXPECT_EQ(2, Scans);
}

TEST(FContentBrowserRefreshCoordinatorTests, ReentrantChangesRemainInTheNextBatch)
{
	FContentBrowserRefreshCoordinator Coordinator;
	std::vector<std::string> Paths;
	int Scans = 0;
	Coordinator.ObserveMountedContent({.Changes = {{EContentChangeKind::Added, {}, "/Content/First"}}});
	auto Scan = [&] {
		if (++Scans == 1) Coordinator.ObserveMountedContent({.Changes = {{EContentChangeKind::Added, {}, "/Content/Second"}}});
		return std::expected<void, UnifiedError::FError>{};
	};
	auto Refresh = [&](const FContentChangeBatch& Batch) { ASSERT_EQ(1u, Batch.Changes.size()); Paths.push_back(Batch.Changes.front().NewPhysicalPath); };
	EXPECT_TRUE(Coordinator.Synchronize(Scan, Refresh));
	ASSERT_EQ(1u, Paths.size()); EXPECT_EQ("/Content/First", Paths.front());
	EXPECT_TRUE(Coordinator.Synchronize(Scan, Refresh));
	ASSERT_EQ(2u, Paths.size()); EXPECT_EQ("/Content/Second", Paths.back()); EXPECT_EQ(2, Scans);
}

TEST(FContentBrowserRefreshCoordinatorTests, ExplicitReconciliationPreservesReentrantChanges)
{
	FContentBrowserRefreshCoordinator Coordinator;
	int Scans = 0, Refreshes = 0;
	auto Scan = [&] {
		if (++Scans == 1) Coordinator.ObserveMountedContent({.Changes = {{EContentChangeKind::Added, {}, "/Content/DuringRefresh"}}});
		return std::expected<void, UnifiedError::FError>{};
	};
	auto Refresh = [&](const FContentChangeBatch& Batch) {
		if (++Refreshes == 1) { EXPECT_TRUE(Batch.bFullRefresh); EXPECT_TRUE(Batch.Changes.empty()); }
		else { ASSERT_EQ(1u, Batch.Changes.size()); EXPECT_EQ("/Content/DuringRefresh", Batch.Changes.front().NewPhysicalPath); }
	};
	EXPECT_TRUE(Coordinator.ReconcileExplicitly(Scan, Refresh));
	EXPECT_EQ(1, Refreshes);
	EXPECT_TRUE(Coordinator.Synchronize(Scan, Refresh));
	EXPECT_EQ(2, Scans); EXPECT_EQ(2, Refreshes);
}
