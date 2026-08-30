#include <gtest/gtest.h>

#include "AssetMaintenance/CanonicalResave.h"
#include "AssetMaintenance/CompatibilityAudit.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"

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
			const std::array Definitions{Durin::FMountPoint{
				.VirtualRoot = "/Maintenance/",
				.Owner = Durin::EMountOwner::Test,
				.Root = Durin::Testing::GetTestWorkDirectory(),
				.bAutoScan = false}};
			Mounts = std::make_unique<Durin::Testing::FScopedMountRegistryFixture>(Definitions);
			ASSERT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		}

		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture> Mounts;
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
	const std::string Report = Durin::Asset::SerializeAssetCompatibilityReport(Records);

	Durin::FJsonDocument Document;
	ASSERT_TRUE(Document.Parse(Report));
	const Durin::FJsonNodeView Root = Document.GetRootView();
	EXPECT_EQ(Root.GetView("schemaVersion").GetUInt(), 3u);
	const Durin::FJsonNodeView Packages = Root.GetView("packages");
	ASSERT_EQ(Packages.Num(), 2u);
	EXPECT_EQ(Packages.GetView(0).GetView("packagePath").GetString(), "/Maintenance/A");
	EXPECT_EQ(Packages.GetView(1).GetView("packagePath").GetString(), "/Maintenance/B");
}

TEST_F(FAssetMaintenanceContractTests, CoreJsonSerializationPreservesControlCharacters)
{
	const std::string ControlCharacters = std::string("before\b\f") + '\x01' + "after";
	Durin::Asset::FAssetPackageCompatibilityRecord CompatibilityRecord{
		.PackagePath = MakePath("/Maintenance/Compatibility"),
		.PhysicalPath = ControlCharacters};
	CompatibilityRecord.Findings.push_back({.Diagnostic = ControlCharacters});

	Durin::FJsonDocument CompatibilityDocument;
	ASSERT_TRUE(CompatibilityDocument.Parse(
		Durin::Asset::SerializeAssetCompatibilityReport(
			std::array{CompatibilityRecord})));
	const Durin::FJsonNodeView CompatibilityPackage =
		CompatibilityDocument.GetRootView().GetView("packages").GetView(0);
	EXPECT_EQ(CompatibilityPackage.GetView("physicalPath").GetString(), ControlCharacters);
	EXPECT_EQ(CompatibilityPackage.GetView("findings").GetView(0)
		.GetView("diagnostic").GetString(), ControlCharacters);

	Durin::Asset::FAssetCanonicalResaveApplyResult ApplyResult;
	ApplyResult.Diagnostic = ControlCharacters;
	ApplyResult.Plan.Packages.push_back({
		.PackagePath = MakePath("/Maintenance/Canonical"),
		.PhysicalPath = ControlCharacters,
		.Diagnostics = {ControlCharacters}});
	Durin::FJsonDocument CanonicalDocument;
	ASSERT_TRUE(CanonicalDocument.Parse(
		Durin::Asset::SerializeAssetCanonicalResaveApplyReport(ApplyResult)));
	const Durin::FJsonNodeView CanonicalRoot = CanonicalDocument.GetRootView();
	EXPECT_EQ(CanonicalRoot.GetView("diagnostic").GetString(), ControlCharacters);
	const Durin::FJsonNodeView CanonicalPackage =
		CanonicalRoot.GetView("packages").GetView(0);
	EXPECT_EQ(CanonicalPackage.GetView("physicalPath").GetString(), ControlCharacters);
	EXPECT_EQ(CanonicalPackage.GetView("diagnostics").GetView(0).GetString(),
		ControlCharacters);
}
