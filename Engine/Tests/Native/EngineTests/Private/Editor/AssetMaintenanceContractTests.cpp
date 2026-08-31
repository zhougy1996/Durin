#include <gtest/gtest.h>

#include "AssetMaintenance/CanonicalResave.h"
#include "AssetMaintenance/CompatibilityAudit.h"
#include "AssetMaintenance/PackageFormatMigration.h"
#include "AssetRegistry/PackageFormat.h"
#include "DObject/PackageFormat.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"

#include "NativeTestSupport.h"

namespace
{
	namespace Package = Durin::ObjectPackage;

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

	auto MakeMigrationFixture(std::string_view PackageName)
		-> Durin::ObjectPackage::FLinkerTables
	{
		const Package::FSerializedType BulkType{.Kind = Package::EValueKind::BulkData};
		Package::FSerializedValue Bulk;
		Bulk.Bytes = {std::byte{0xaa}, std::byte{0xbb}, std::byte{0xcc}};
		Bulk.BulkElementSize = 1;
		Bulk.BulkAlignment = 4;
		Bulk.BulkStorage = Package::EBulkStorageKind::External;
		Package::FLinkerTables Linker;
		Linker.Summary.PackageName = std::string(PackageName);
		Linker.Summary.AssetClass = "Example::Migration";
		Package::FPackageIndex::TryExport(0, Linker.Summary.MainExport);
		Linker.Schemas = {{.QualifiedName = "Example::Migration",
			.Fields = {{.Name = "Payload", .Type = BulkType}}}};
		Linker.Exports = {{.ObjectName = std::string(
			PackageName.substr(PackageName.find_last_of('/') + 1)),
			.ClassName = "Example::Migration",
			.Properties = {{.DeclaringType = "Example::Migration", .FieldName = "Payload",
				.Type = BulkType, .Value = std::move(Bulk)}}}};
		return Linker;
	}

	auto WriteMigrationFixture(std::string_view PackageName,
		const std::filesystem::path& MainPath, const std::filesystem::path& BulkPath)
		-> void
	{
		std::vector<std::byte> Main;
		std::vector<std::byte> Bulk;
		ASSERT_TRUE(Durin::ObjectPackage::WritePackageV8(
			MakeMigrationFixture(PackageName), Main, Bulk));
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Main, MainPath));
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Bulk, BulkPath));
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

TEST_F(FAssetMaintenanceContractTests, CanonicalResaveTargetsCurrentPackageFormat)
{
	const Durin::Asset::FAssetCanonicalResavePlan Plan =
		Durin::Asset::PlanAssetCanonicalResaves({}, {});

	EXPECT_EQ(Plan.TargetFormatVersion,
		Durin::Asset::AssetPackageV9FormatVersion);
}

TEST_F(FAssetMaintenanceContractTests, PackageFormatMigrationPreviewIsDeterministic)
{
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory();
	const auto APath = Root / "MigrationA.dasset";
	const auto ABulkPath = Root / "MigrationA.dbulk";
	const auto BPath = Root / "MigrationB.dasset";
	const auto BBulkPath = Root / "MigrationB.dbulk";
	WriteMigrationFixture("/Maintenance/MigrationA", APath, ABulkPath);
	WriteMigrationFixture("/Maintenance/MigrationB", BPath, BBulkPath);
	const std::array Inputs{
		Durin::Asset::FPackageFormatMigrationInput{
			.PackagePath = MakePath("/Maintenance/MigrationB"),
			.MainPath = BPath, .BulkPath = BBulkPath},
		Durin::Asset::FPackageFormatMigrationInput{
			.PackagePath = MakePath("/Maintenance/MigrationA"),
			.MainPath = APath, .BulkPath = ABulkPath}};

	const auto First = Durin::Asset::PlanPackageFormatMigration(Inputs);
	const auto Second = Durin::Asset::PlanPackageFormatMigration(Inputs);
	ASSERT_EQ(First.Packages.size(), 2u);
	EXPECT_EQ(First.Packages[0].Input.PackagePath.ToString(), "/Maintenance/MigrationA");
	EXPECT_EQ(First.Packages[0].Status,
		Durin::Asset::EPackageFormatMigrationStatus::Ready);
	EXPECT_EQ(First.Packages[0].TargetFingerprint,
		Second.Packages[0].TargetFingerprint);
	EXPECT_EQ(Durin::Asset::SerializePackageFormatMigrationPlanReport(First),
		Durin::Asset::SerializePackageFormatMigrationPlanReport(Second));
}

TEST_F(FAssetMaintenanceContractTests, PackageFormatMigrationRejectsStalePlan)
{
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory();
	const auto MainPath = Root / "Stale.dasset";
	const auto BulkPath = Root / "Stale.dbulk";
	WriteMigrationFixture("/Maintenance/Stale", MainPath, BulkPath);
	const std::array Inputs{Durin::Asset::FPackageFormatMigrationInput{
		.PackagePath = MakePath("/Maintenance/Stale"),
		.MainPath = MainPath, .BulkPath = BulkPath}};
	auto Plan = Durin::Asset::PlanPackageFormatMigration(Inputs);
	std::vector<std::byte> Changed;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Changed, MainPath));
	Changed.back() ^= std::byte{1};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFileAtomically(Changed, MainPath));

	const auto Result = Durin::Asset::ApplyPackageFormatMigration(std::move(Plan));
	EXPECT_EQ(Result.Status, Durin::Asset::EPackageFormatMigrationApplyStatus::Failed);
	ASSERT_EQ(Result.Plan.Packages.size(), 1u);
	EXPECT_EQ(Result.Plan.Packages[0].Status,
		Durin::Asset::EPackageFormatMigrationStatus::Stale);
	std::vector<std::byte> After;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(After, MainPath));
	EXPECT_EQ(After, Changed);
}

TEST_F(FAssetMaintenanceContractTests, PackageFormatMigrationRollsBackPartialPublication)
{
	const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory();
	const auto MainPath = Root / "Rollback.dasset";
	const auto BulkPath = Root / "Rollback.dbulk";
	WriteMigrationFixture("/Maintenance/Rollback", MainPath, BulkPath);
	std::vector<std::byte> BeforeMain;
	std::vector<std::byte> BeforeBulk;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeMain, MainPath));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(BeforeBulk, BulkPath));
	const std::array Inputs{Durin::Asset::FPackageFormatMigrationInput{
		.PackagePath = MakePath("/Maintenance/Rollback"),
		.MainPath = MainPath, .BulkPath = BulkPath}};
	auto Plan = Durin::Asset::PlanPackageFormatMigration(Inputs);
	Durin::Asset::FPackageFormatMigrationApplyOptions Options;
	Options.ShouldFail = [](Durin::Asset::EPackageFormatMigrationApplyPhase Phase, size_t) {
		return Phase == Durin::Asset::EPackageFormatMigrationApplyPhase::PublishBulk;
	};
	const auto Result = Durin::Asset::ApplyPackageFormatMigration(
		std::move(Plan), Options);
	EXPECT_EQ(Result.Status, Durin::Asset::EPackageFormatMigrationApplyStatus::Failed);
	std::vector<std::byte> AfterMain;
	std::vector<std::byte> AfterBulk;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterMain, MainPath));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterBulk, BulkPath));
	EXPECT_EQ(AfterMain, BeforeMain);
	EXPECT_EQ(AfterBulk, BeforeBulk);
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
