#include <gtest/gtest.h>

#include "AssetMaintenance/CompatibilityAudit.h"
#include "Misc/Paths.h"

#include "NativeTestSupport.h"

namespace
{
	auto MakePath(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Path));
		return Path;
	}

	auto MakeProbeResult(const Durin::Asset::FAssetPackageCompatibilityProbeInput& Input)
		-> Durin::Asset::FAssetPackageCompatibilityProbeResult
	{
		return {.Record = Durin::Asset::FAssetPackageCompatibilityRecord{
			.PackagePath = Input.PackagePath,
			.PhysicalPath = Input.PhysicalPath,
			.Inspection = Durin::Asset::EAssetCompatibilityInspection::Ready,
			.Compatibility = Durin::Asset::EAssetPackageCompatibility::Compatible}};
	}

	class FAssetMaintenanceContractTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			const std::array Definitions{Durin::PathUtilities::FMountPoint{
				.VirtualRoot = "/Maintenance/",
				.Owner = Durin::PathUtilities::EMountOwner::Test,
				.Root = Durin::Testing::GetTestWorkDirectory(),
				.bAutoScan = false}};
			Mounts = std::make_unique<Durin::PathUtilities::FScopedMountRegistryFixture>(Definitions);
			ASSERT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		}

		std::unique_ptr<Durin::PathUtilities::FScopedMountRegistryFixture> Mounts;
	};
}

TEST_F(FAssetMaintenanceContractTests, BatchAuditSortsInputsAndStreamsDeterministicProgress)
{
	const std::array Inputs{
		Durin::Asset::FAssetPackageCompatibilityProbeInput{
			.PackagePath = MakePath("/Maintenance/B"), .PhysicalPath = "B.dasset"},
		Durin::Asset::FAssetPackageCompatibilityProbeInput{
			.PackagePath = MakePath("/Maintenance/A"), .PhysicalPath = "A.dasset"}};
	std::vector<std::string> Published;
	std::vector<uint64> Progress;
	const auto Result = Durin::Asset::RunAssetCompatibilityAudit(
		Inputs, {}, {},
		[&](const Durin::Asset::FAssetPackageCompatibilityRecord& Record,
			uint64 Completed, uint64 Total) {
			Published.push_back(Record.PackagePath.ToString());
			Progress.push_back(Completed);
			EXPECT_EQ(Total, Inputs.size());
		},
		[](const auto& Input, const auto&, const auto&) {
			return MakeProbeResult(Input);
		});

	EXPECT_EQ(Result.Status, Durin::Asset::EAssetCompatibilityAuditStatus::Completed);
	ASSERT_EQ(Result.Records.size(), 2u);
	EXPECT_EQ(Result.Records[0].PackagePath.ToString(), "/Maintenance/A");
	EXPECT_EQ(Result.Records[1].PackagePath.ToString(), "/Maintenance/B");
	EXPECT_EQ(Published, (std::vector<std::string>{"/Maintenance/A", "/Maintenance/B"}));
	EXPECT_EQ(Progress, (std::vector<uint64>{1, 2}));
}

TEST_F(FAssetMaintenanceContractTests, BatchAuditCancelsBeforeAdmittingTheNextPackage)
{
	const std::array Inputs{
		Durin::Asset::FAssetPackageCompatibilityProbeInput{
			.PackagePath = MakePath("/Maintenance/A"), .PhysicalPath = "A.dasset"},
		Durin::Asset::FAssetPackageCompatibilityProbeInput{
			.PackagePath = MakePath("/Maintenance/B"), .PhysicalPath = "B.dasset"}};
	uint32 CancellationChecks = 0;
	const auto Result = Durin::Asset::RunAssetCompatibilityAudit(
		Inputs, {}, [&] { return ++CancellationChecks > 1; }, {},
		[](const auto& Input, const auto&, const auto&) {
			return MakeProbeResult(Input);
		});

	EXPECT_EQ(Result.Status, Durin::Asset::EAssetCompatibilityAuditStatus::Cancelled);
	ASSERT_EQ(Result.Records.size(), 1u);
	EXPECT_EQ(Result.Records.front().PackagePath.ToString(), "/Maintenance/A");
}

TEST_F(FAssetMaintenanceContractTests, CompatibilityReportKeepsStableSchemaAndPathOrder)
{
	const std::array Records{
		Durin::Asset::FAssetPackageCompatibilityRecord{
			.PackagePath = MakePath("/Maintenance/B"), .PhysicalPath = "B.dasset",
			.Inspection = Durin::Asset::EAssetCompatibilityInspection::Ready,
			.Compatibility = Durin::Asset::EAssetPackageCompatibility::Compatible},
		Durin::Asset::FAssetPackageCompatibilityRecord{
			.PackagePath = MakePath("/Maintenance/A"), .PhysicalPath = "A.dasset",
			.Inspection = Durin::Asset::EAssetCompatibilityInspection::Ready,
			.Compatibility = Durin::Asset::EAssetPackageCompatibility::Incompatible}};
	const std::string Report = Durin::Asset::SerializeAssetCompatibilityReportV1(Records);

	EXPECT_TRUE(Report.starts_with("{\"schemaVersion\":3,\"packages\":["));
	EXPECT_LT(Report.find("/Maintenance/A"), Report.find("/Maintenance/B"));
}
