#include "Asset/AssetUpgradeAuditService.h"

#include "EngineTestSupport.h"
#include "Misc/Paths.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeAssetData(std::string_view Path) -> Durin::Asset::FAssetData
	{
		InitializeDObjectSystem();
		static const bool bMounted = [] {
			Durin::PathUtilities::RegisterMountPoint(
				"/UpgradeAudit/",
				std::filesystem::path(DURIN_TEST_WORK_DIR).generic_string() + "/");
			return true;
		}();
		(void)bMounted;
		Durin::FAssetPath AssetPath;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Path, AssetPath));
		return {
			.PackagePath = std::move(AssetPath),
			.AssetClassName = "Durin::DObject"};
	}

	struct FAuditHarness
	{
		Durin::uint64 Revision = 1;
		double Seconds = 0.0;
		double AuditCostSeconds = 0.0;
		std::vector<Durin::Asset::FAssetData> Assets;
		std::vector<std::string> AuditOrder;

		auto MakeDependencies() -> Durin::FAssetUpgradeAuditServiceDependencies
		{
			return {
				.GetRegistryRevision = [this] {
					return Revision;
				},
				.SnapshotAssets = [this] {
					return Assets;
				},
				.AuditPackage = [this](
					const Durin::Asset::FAssetData& Data,
					Durin::Asset::FAssetPackageAuditReport& Report) {
					AuditOrder.push_back(Data.PackagePath.ToString());
					Seconds += AuditCostSeconds;
					Report.State = Durin::Asset::EAssetPackageAuditState::UpToDate;
					return Durin::Asset::FAssetResult{};
				},
				.GetSeconds = [this] {
					return Seconds;
				}};
		}
	};
}

TEST(FAssetUpgradeAuditServiceTests, PublishesDeterministicIncrementalSnapshots)
{
	FAuditHarness Harness;
	Harness.Assets = {
		MakeAssetData("/UpgradeAudit/C"),
		MakeAssetData("/UpgradeAudit/A"),
		MakeAssetData("/UpgradeAudit/B")};
	Durin::FAssetUpgradeAuditService Service(Harness.MakeDependencies(), 2, 1000.0);

	Service.Start();
	auto Snapshot = Service.GetSnapshot();
	ASSERT_EQ(Snapshot->State, Durin::EAssetUpgradeAuditServiceState::Auditing);
	ASSERT_EQ(Snapshot->Session.Packages.size(), 3);
	EXPECT_EQ(Snapshot->Session.Packages[0].PackagePath.ToString(), "/UpgradeAudit/A");
	EXPECT_EQ(Snapshot->Session.Packages[1].PackagePath.ToString(), "/UpgradeAudit/B");
	EXPECT_EQ(Snapshot->Session.Packages[2].PackagePath.ToString(), "/UpgradeAudit/C");
	EXPECT_EQ(Snapshot->Session.Progress.Completed, 0);

	Service.Tick();
	Snapshot = Service.GetSnapshot();
	EXPECT_EQ(Snapshot->State, Durin::EAssetUpgradeAuditServiceState::Auditing);
	EXPECT_EQ(Snapshot->Session.Progress.Completed, 2);
	EXPECT_EQ(Harness.AuditOrder, (std::vector<std::string>{"/UpgradeAudit/A", "/UpgradeAudit/B"}));

	Service.Tick();
	Snapshot = Service.GetSnapshot();
	EXPECT_EQ(Snapshot->State, Durin::EAssetUpgradeAuditServiceState::Completed);
	EXPECT_EQ(Snapshot->Session.Progress.Completed, 3);
	EXPECT_EQ(Harness.AuditOrder.back(), "/UpgradeAudit/C");
}

TEST(FAssetUpgradeAuditServiceTests, CompletesOneOverBudgetPackageAtomically)
{
	FAuditHarness Harness;
	Harness.Assets = {
		MakeAssetData("/UpgradeAudit/A"),
		MakeAssetData("/UpgradeAudit/B")};
	Harness.AuditCostSeconds = 0.003;
	Durin::FAssetUpgradeAuditService Service(Harness.MakeDependencies(), 4, 2.0);

	Service.Start();
	Service.Tick();
	auto Snapshot = Service.GetSnapshot();
	ASSERT_EQ(Snapshot->Session.Progress.Completed, 1);
	EXPECT_EQ(Snapshot->State, Durin::EAssetUpgradeAuditServiceState::Auditing);
	EXPECT_NEAR(Snapshot->Session.Packages[0].AuditDurationMilliseconds, 3.0, 0.0001);

	Service.Tick();
	Snapshot = Service.GetSnapshot();
	EXPECT_EQ(Snapshot->Session.Progress.Completed, 2);
	EXPECT_EQ(Snapshot->State, Durin::EAssetUpgradeAuditServiceState::Completed);
}

TEST(FAssetUpgradeAuditServiceTests, PausesResumesAndCancelsWithoutDiscardingProgress)
{
	FAuditHarness Harness;
	Harness.Assets = {
		MakeAssetData("/UpgradeAudit/A"),
		MakeAssetData("/UpgradeAudit/B"),
		MakeAssetData("/UpgradeAudit/C")};
	Durin::FAssetUpgradeAuditService Service(Harness.MakeDependencies(), 1, 1000.0);

	Service.Start();
	Service.Tick();
	Service.Pause();
	Service.Tick();
	EXPECT_EQ(Service.GetSnapshot()->Session.Progress.Completed, 1);
	EXPECT_EQ(Service.GetSnapshot()->State, Durin::EAssetUpgradeAuditServiceState::Paused);

	Service.Resume();
	Service.Tick();
	Service.Cancel();
	const auto Snapshot = Service.GetSnapshot();
	EXPECT_EQ(Snapshot->State, Durin::EAssetUpgradeAuditServiceState::Cancelled);
	EXPECT_EQ(Snapshot->Session.Progress.Completed, 2);
	ASSERT_EQ(Snapshot->Session.Packages.size(), 3);
	EXPECT_EQ(Snapshot->Session.Packages[2].State, Durin::Asset::EAssetPackageAuditState::NotAudited);
}

TEST(FAssetUpgradeAuditServiceTests, RegistryRevisionStartsANewGeneration)
{
	FAuditHarness Harness;
	Harness.Assets = {
		MakeAssetData("/UpgradeAudit/A"),
		MakeAssetData("/UpgradeAudit/B")};
	Durin::FAssetUpgradeAuditService Service(Harness.MakeDependencies(), 1, 1000.0);

	Service.Start();
	Service.Tick();
	const Durin::uint64 InitialGeneration = Service.GetSnapshot()->Generation;
	Harness.Assets = {MakeAssetData("/UpgradeAudit/C")};
	++Harness.Revision;

	Service.Tick();
	const auto Snapshot = Service.GetSnapshot();
	EXPECT_GT(Snapshot->Generation, InitialGeneration);
	EXPECT_EQ(Snapshot->Session.RegistryRevision, Harness.Revision);
	EXPECT_EQ(Snapshot->Session.Progress.Completed, 0);
	ASSERT_EQ(Snapshot->Session.Packages.size(), 1);
	EXPECT_EQ(Snapshot->Session.Packages[0].PackagePath.ToString(), "/UpgradeAudit/C");
	EXPECT_EQ(Snapshot->State, Durin::EAssetUpgradeAuditServiceState::Auditing);
}

TEST(FAssetUpgradeAuditServiceTests, ShutdownIsTerminal)
{
	FAuditHarness Harness;
	Harness.Assets = {MakeAssetData("/UpgradeAudit/A")};
	Durin::FAssetUpgradeAuditService Service(Harness.MakeDependencies());

	Service.Start();
	Service.Shutdown();
	Service.Start();
	Service.Tick();

	EXPECT_EQ(Service.GetSnapshot()->State, Durin::EAssetUpgradeAuditServiceState::Shutdown);
	EXPECT_EQ(Harness.AuditOrder.size(), 0);
}
