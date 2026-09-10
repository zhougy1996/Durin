#include <gtest/gtest.h>

#include "AssetMaintenance/CanonicalResave.h"
#include "AssetMaintenance/CompatibilityAudit.h"
#include "DObject/PackageFormat.h"
#include "DObject/PackageFormat.h"
#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"

#include "NativeTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "EngineTestSupport.h"
#include "Texture/Texture2D.h"
#include "Asset/PackageSerialization.h"
#include "Misc/FileTime.h"

namespace
{
	namespace Package = Durin::ObjectPackage;

	auto MakePath(std::string_view Value) -> Durin::FPackagePath
	{
		Durin::FPackagePath Path;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Path));
		return Path;
	}

	auto MakeProbeResult(const Durin::FAssetPackageCompatibilityProbeInput& Input)
		-> Durin::FAssetPackageCompatibilityProbeResult
	{
		return {.Record = Durin::FAssetPackageCompatibilityRecord{
			.PackagePath = Input.PackagePath,
			.PhysicalPath = Input.PhysicalPath,
			.Inspection = Durin::EAssetCompatibilityInspection::Ready,
			.Compatibility = Durin::EAssetPackageCompatibility::Compatible}};
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
				.bAutoScan = false, .bContentWritable = true}};
			Mounts = std::make_unique<Durin::Testing::FScopedMountRegistryFixture>(Definitions);
			ASSERT_TRUE(Mounts->IsValid()) << Mounts->GetError();
		}

		std::unique_ptr<Durin::Testing::FScopedMountRegistryFixture> Mounts;
	};
}

TEST_F(FAssetMaintenanceContractTests, BatchAuditSortsInputsAndStreamsDeterministicProgress)
{
	const std::array Inputs{
		Durin::FAssetPackageCompatibilityProbeInput{
			.PackagePath = MakePath("/Maintenance/B"), .PhysicalPath = "B.dasset"},
		Durin::FAssetPackageCompatibilityProbeInput{
			.PackagePath = MakePath("/Maintenance/A"), .PhysicalPath = "A.dasset"}};
	std::vector<std::string> Published;
	std::vector<uint64> Progress;
	const auto Result = Durin::RunAssetCompatibilityAudit(
		Inputs, {}, {},
		[&](const Durin::FAssetPackageCompatibilityRecord& Record,
			uint64 Completed, uint64 Total) {
			Published.push_back(Record.PackagePath.ToString());
			Progress.push_back(Completed);
			EXPECT_EQ(Total, Inputs.size());
		},
		[](const auto& Input, const auto&, const auto&) {
			return MakeProbeResult(Input);
		});

	EXPECT_EQ(Result.Status, Durin::EAssetCompatibilityAuditStatus::Completed);
	ASSERT_EQ(Result.Records.size(), 2u);
	EXPECT_EQ(Result.Records[0].PackagePath.ToString(), "/Maintenance/A");
	EXPECT_EQ(Result.Records[1].PackagePath.ToString(), "/Maintenance/B");
	EXPECT_EQ(Published, (std::vector<std::string>{"/Maintenance/A", "/Maintenance/B"}));
	EXPECT_EQ(Progress, (std::vector<uint64>{1, 2}));
}

TEST_F(FAssetMaintenanceContractTests, BatchAuditCancelsBeforeAdmittingTheNextPackage)
{
	const std::array Inputs{
		Durin::FAssetPackageCompatibilityProbeInput{
			.PackagePath = MakePath("/Maintenance/A"), .PhysicalPath = "A.dasset"},
		Durin::FAssetPackageCompatibilityProbeInput{
			.PackagePath = MakePath("/Maintenance/B"), .PhysicalPath = "B.dasset"}};
	uint32 CancellationChecks = 0;
	const auto Result = Durin::RunAssetCompatibilityAudit(
		Inputs, {}, [&] { return ++CancellationChecks > 1; }, {},
		[](const auto& Input, const auto&, const auto&) {
			return MakeProbeResult(Input);
		});

	EXPECT_EQ(Result.Status, Durin::EAssetCompatibilityAuditStatus::Cancelled);
	ASSERT_EQ(Result.Records.size(), 1u);
	EXPECT_EQ(Result.Records.front().PackagePath.ToString(), "/Maintenance/A");
}

TEST_F(FAssetMaintenanceContractTests, CompatibilityReportKeepsStableSchemaAndPathOrder)
{
	const std::array Records{
		Durin::FAssetPackageCompatibilityRecord{
			.PackagePath = MakePath("/Maintenance/B"), .PhysicalPath = "B.dasset",
			.Inspection = Durin::EAssetCompatibilityInspection::Ready,
			.Compatibility = Durin::EAssetPackageCompatibility::Compatible},
		Durin::FAssetPackageCompatibilityRecord{
			.PackagePath = MakePath("/Maintenance/A"), .PhysicalPath = "A.dasset",
			.Inspection = Durin::EAssetCompatibilityInspection::Ready,
			.Compatibility = Durin::EAssetPackageCompatibility::Incompatible}};
	const std::string Report = Durin::SerializeAssetCompatibilityReport(Records);

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
	Durin::FAssetPackageCompatibilityRecord CompatibilityRecord{
		.PackagePath = MakePath("/Maintenance/Compatibility"),
		.PhysicalPath = ControlCharacters};
	CompatibilityRecord.Findings.push_back({.Diagnostic = ControlCharacters});

	Durin::FJsonDocument CompatibilityDocument;
	ASSERT_TRUE(CompatibilityDocument.Parse(
		Durin::SerializeAssetCompatibilityReport(
			std::array{CompatibilityRecord})));
	const Durin::FJsonNodeView CompatibilityPackage =
		CompatibilityDocument.GetRootView().GetView("packages").GetView(0);
	EXPECT_EQ(CompatibilityPackage.GetView("physicalPath").GetString(), ControlCharacters);
	EXPECT_EQ(CompatibilityPackage.GetView("findings").GetView(0)
		.GetView("diagnostic").GetString(), ControlCharacters);

	Durin::FAssetCanonicalResaveApplyResult ApplyResult;
	ApplyResult.Diagnostic = ControlCharacters;
	ApplyResult.Plan.Packages.push_back({
		.PackagePath = MakePath("/Maintenance/Canonical"),
		.PhysicalPath = ControlCharacters,
		.Diagnostics = {ControlCharacters}});
	Durin::FJsonDocument CanonicalDocument;
	ASSERT_TRUE(CanonicalDocument.Parse(
		Durin::SerializeAssetCanonicalResaveApplyReport(ApplyResult)));
	const Durin::FJsonNodeView CanonicalRoot = CanonicalDocument.GetRootView();
	EXPECT_EQ(CanonicalRoot.GetView("diagnostic").GetString(), ControlCharacters);
	const Durin::FJsonNodeView CanonicalPackage =
		CanonicalRoot.GetView("packages").GetView(0);
	EXPECT_EQ(CanonicalPackage.GetView("physicalPath").GetString(), ControlCharacters);
	EXPECT_EQ(CanonicalPackage.GetView("diagnostics").GetView(0).GetString(),
		ControlCharacters);
}

TEST_F(FAssetMaintenanceContractTests, RecompressionSelectsCurrentPackagesAndHonorsCancellation)
{
	using namespace Durin;
	FAssetPackageCompatibilityRecord Record{
		.PackagePath = MakePath("/Maintenance/Current"),
		.Inspection = EAssetCompatibilityInspection::Ready,
		.Compatibility = EAssetPackageCompatibility::Compatible};
	const std::array Records{Record};
	const auto Ordinary = PlanAssetCanonicalResaves(Records, {.bWholeProject = true});
	ASSERT_EQ(Ordinary.Packages.size(), 1u);
	EXPECT_EQ(Ordinary.Packages[0].Status, EAssetCanonicalResavePackageStatus::Skipped);
	const auto Plan = PlanAssetCanonicalResaves(Records,
		{.bWholeProject = true, .bRecompressTextureSources = true});
	EXPECT_EQ(Plan.Packages[0].Status, EAssetCanonicalResavePackageStatus::Ready);
	const auto Cancelled = ApplyAssetCanonicalResaves(Plan, {}, {}, [] { return true; });
	EXPECT_EQ(Cancelled.Status, EAssetCanonicalResaveApplyStatus::Cancelled);
	EXPECT_TRUE(Cancelled.ChangedPaths.empty());
}

TEST_F(FAssetMaintenanceContractTests, RecompressionPreviewAndSaveFailurePreserveSourceAndFiles)
{
	using namespace Durin;
	InitializeDObjectSystem();
	const auto Path = MakePath("/Maintenance/TextureStorage");
	DTexture2D* Texture = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Texture));
	Image::FImage Image;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 1024, .Height = 1024,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(1024 * 1024 * 4, std::byte{17}), Image));
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 4, 0, ETextureSourceCompression::Raw));
	Texture->SetSource(Source);
	auto Platform = std::make_unique<FTexturePlatformData>();
	Platform->PixelFormat = EPixelFormat::RGBA8_UNORM;
	Platform->Mips.push_back({.Pixels = FByteBuffer(4, std::byte{17}),
		.Width = 1, .Height = 1, .RowPitch = 4});
	Texture->SetPlatformData(std::move(Platform));
	const auto Saved = SavePackage(Texture->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	const auto Entry = FindAssetExact(Path);
	ASSERT_TRUE(Entry);
	FByteBuffer Before;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Before, Entry->PhysicalPath));
	std::filesystem::path BulkPath = Entry->PhysicalPath;
	BulkPath.replace_extension(".dbulk");
	FByteBuffer BulkBefore;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(BulkBefore, BulkPath));
	const auto MakePlan = [&]() {
		const FAssetPackageCompatibilityRecord Record{
			.PackagePath = Path, .PhysicalPath = Entry->PhysicalPath,
			.Fingerprint = {.FileSize = Before.size(),
				.LastWriteTimeTicks = FileTime::ToStableTicks(std::filesystem::last_write_time(Entry->PhysicalPath)),
				.ContentHash = FXxHash128::HashBuffer(Before)},
			.Inspection = EAssetCompatibilityInspection::Ready,
			.Compatibility = EAssetPackageCompatibility::Compatible};
		return PlanAssetCanonicalResaves(std::array{Record},
			{.bWholeProject = true, .bRecompressTextureSources = true});
	};
	const auto Preview = ApplyAssetCanonicalResaves(MakePlan(), {}, {.bPreview = true});
	ASSERT_EQ(Preview.Status, EAssetCanonicalResaveApplyStatus::Succeeded) << Preview.Diagnostic;
	ASSERT_EQ(Preview.Plan.Packages[0].TextureSources.size(), 1u);
	EXPECT_TRUE(Preview.Plan.Packages[0].TextureSources[0].bChanged);
	EXPECT_EQ(Texture->GetSource().GetCompression(), ETextureSourceCompression::Raw);
	for (const auto Failure : {EAssetCanonicalResaveApplyPhase::SerializePackage,
		EAssetCanonicalResaveApplyPhase::PublishPackage, EAssetCanonicalResaveApplyPhase::VerifyPackage})
	{
		const auto Failed = ApplyAssetCanonicalResaves(MakePlan(), {},
			{.ShouldFail = [Failure](auto Phase, size_t) { return Phase == Failure; }});
		EXPECT_EQ(Failed.Status, EAssetCanonicalResaveApplyStatus::Failed) << Failed.Diagnostic;
		EXPECT_EQ(Texture->GetSource().GetCompression(), ETextureSourceCompression::Raw);
		EXPECT_EQ(Texture->GetSource().GetIdentity(), Source.GetIdentity());
		EXPECT_EQ(Texture->GetSource().GetBulkData().GetInstanceId(), Source.GetBulkData().GetInstanceId());
		FByteBuffer After, BulkAfter;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(After, Entry->PhysicalPath));
		ASSERT_TRUE(FFileHelper::LoadFileToArray(BulkAfter, BulkPath));
		EXPECT_EQ(After, Before);
		EXPECT_EQ(BulkAfter, BulkBefore);
	}
	const auto Applied = ApplyAssetCanonicalResaves(MakePlan(), {});
	ASSERT_EQ(Applied.Status, EAssetCanonicalResaveApplyStatus::Succeeded) << Applied.Diagnostic;
	EXPECT_EQ(Texture->GetSource().GetCompression(), ETextureSourceCompression::Zstd);
	EXPECT_FALSE(std::filesystem::exists(BulkPath));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Before, Entry->PhysicalPath));
	const auto Repeated = ApplyAssetCanonicalResaves(MakePlan(), {});
	ASSERT_EQ(Repeated.Status, EAssetCanonicalResaveApplyStatus::Succeeded);
	EXPECT_TRUE(Repeated.ChangedPaths.empty());
	FByteBuffer AfterRepeat;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(AfterRepeat, Entry->PhysicalPath));
	EXPECT_EQ(AfterRepeat, Before);
	ASSERT_TRUE(UnloadPackage(Path));
}
