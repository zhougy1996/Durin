#include "AssetMaintenance/PackageMigration.h"

#include "DastV7Fixture.h"
#include "AssetRegistry/ObjectStream.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Serialization/BinaryFormat.h"

#include <gtest/gtest.h>

namespace
{
	namespace Asset = Durin::Asset;
	namespace Stream = Durin::Asset::PackageObjectStream;

	auto Bytes(std::initializer_list<uint8> Values) -> std::vector<std::byte>
	{
		std::vector<std::byte> Result;
		for (uint8 Value : Values) Result.push_back(static_cast<std::byte>(Value));
		return Result;
	}

	auto InlineBulkDescriptor(std::span<const std::byte> Payload)
		-> std::vector<std::byte>
	{
		Durin::FBinaryWriter Writer;
		Writer.WriteU64(1);
		Writer.WriteU8(0);
		Writer.WriteU8(0);
		Writer.WriteU16(1);
		Writer.WriteU32(1);
		Writer.WriteGuid({1, 2, 3, 4});
		Writer.WriteHash128(Durin::FXxHash128::HashBuffer(Payload));
		Writer.WriteU64(Payload.size());
		Writer.WriteU64(Payload.size());
		Writer.WriteU64(0);
		Writer.WriteBytes(Payload);
		return Writer.TakeBytes();
	}

	auto BuildV7Package() -> std::vector<std::byte>
	{
		const auto I32 = Stream::MakeType(Stream::ETypeOpcode::I32);
		const auto Bulk = Stream::MakeType(Stream::ETypeOpcode::BulkData);
		Stream::FPackageInput Input{
			.AssetClass = "Fixture::MigrationAsset",
			.Types = {I32, Bulk},
			.Schemas = {{"Fixture::MigrationAsset", {
				{"Value", I32, 0}, {"Payload", Bulk, 0}}}},
			.Objects = {{"Fixture", {}, "Fixture::MigrationAsset", "Fixture"}},
			.ObjectValues = {{"Fixture", {
				{"Fixture::MigrationAsset", "Value",
					Durin::EDefaultDeltaProvenance::Explicit, {.Signed = 42}},
				{"Fixture::MigrationAsset", "Payload",
					Durin::EDefaultDeltaProvenance::Explicit,
					{.Bytes = InlineBulkDescriptor(Bytes({0xde, 0xad, 0xbe, 0xef}))}},
			}}},
		};
		std::vector<std::byte> ObjectStream;
		Stream::FWriterDiagnostic WriterDiagnostic;
		EXPECT_TRUE(Stream::WritePackage(Input, ObjectStream, &WriterDiagnostic))
			<< WriterDiagnostic.Message;
		std::vector<std::byte> Package;
		EXPECT_TRUE(Durin::Testing::DastV7Fixture::BuildPackageFromObjectStream(
			ObjectStream, Package));
		return Package;
	}

	struct FMigrationFixture
	{
		std::filesystem::path Root;
		std::filesystem::path MainPath;
		Durin::FAssetPath PackagePath;
		std::vector<std::byte> V7;
		Asset::FAssetPackageCompatibilityProbeInput Input;

		FMigrationFixture()
		{
			Durin::Testing::InitializeDObjectSystemForTests();
			Root = Durin::Testing::GetTestWorkDirectory() / "PackageMigration";
			Durin::Testing::RemoveTestWorkDirectory(Root);
			std::filesystem::create_directories(Root);
			Durin::Testing::RegisterMountPointForTests(
				"/Migration/", Root.generic_string() + "/");
			MainPath = Root / "Fixture.dasset";
			EXPECT_TRUE(Durin::FAssetPath::TryCreate(
				"/Migration/Fixture", PackagePath));
			V7 = BuildV7Package();
			EXPECT_TRUE(Durin::FFileHelper::SaveArrayToFile(V7, MainPath));
			Input.PackagePath = PackagePath;
			Input.PhysicalPath = MainPath.generic_string();
			Input.ExpectedFileSize = V7.size();
			Input.ExpectedContentHash = Durin::FXxHash128::HashBuffer(V7);
		}
	};
}

TEST(FPackageMigrationTests, PlanIsDeterministicAndReadOnlyThenApplyIsIdempotent)
{
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	FMigrationFixture Fixture;

	const Asset::FAssetPackageMigrationPlan First =
		Asset::PlanAssetPackageMigrationV8(std::span(&Fixture.Input, 1));
	const Asset::FAssetPackageMigrationPlan Second =
		Asset::PlanAssetPackageMigrationV8(std::span(&Fixture.Input, 1));
	ASSERT_EQ(First.Packages.size(), 1);
	EXPECT_EQ(First.Packages.front().Status,
		Asset::EAssetPackageMigrationStatus::Ready);
	EXPECT_EQ(Asset::SerializeAssetPackageMigrationPlanReport(First),
		Asset::SerializeAssetPackageMigrationPlanReport(Second));
	std::vector<std::byte> AfterPlan;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(AfterPlan, Fixture.MainPath));
	EXPECT_EQ(AfterPlan, Fixture.V7);

	Asset::FAssetPackageMigrationApplyResult Applied =
		Asset::ApplyAssetPackageMigrationV8(First);
	ASSERT_EQ(Applied.Status, Asset::EAssetPackageMigrationApplyStatus::Succeeded)
		<< Applied.Diagnostic;
	ASSERT_EQ(Applied.Plan.Packages.front().Status,
		Asset::EAssetPackageMigrationStatus::Converted);
	ASSERT_EQ(Applied.ChangedPaths.size(), 1);

	std::vector<std::byte> V8;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(V8, Fixture.MainPath));
	Fixture.Input.ExpectedFileSize = V8.size();
	Fixture.Input.ExpectedContentHash = Durin::FXxHash128::HashBuffer(V8);
	const Asset::FAssetPackageMigrationPlan NoOp =
		Asset::PlanAssetPackageMigrationV8(std::span(&Fixture.Input, 1));
	ASSERT_EQ(NoOp.Packages.size(), 1);
	EXPECT_EQ(NoOp.Packages.front().Status,
		Asset::EAssetPackageMigrationStatus::AlreadyV8);
	const auto NoOpApply = Asset::ApplyAssetPackageMigrationV8(NoOp);
	EXPECT_EQ(NoOpApply.Status, Asset::EAssetPackageMigrationApplyStatus::Succeeded);
	EXPECT_TRUE(NoOpApply.ChangedPaths.empty());
}

TEST(FPackageMigrationTests, PublicationFailureRestoresTheCompleteSourceClosure)
{
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	FMigrationFixture Fixture;
	Asset::FAssetPackageMigrationPlan Plan =
		Asset::PlanAssetPackageMigrationV8(std::span(&Fixture.Input, 1));
	ASSERT_EQ(Plan.Packages.front().Status,
		Asset::EAssetPackageMigrationStatus::Ready);
	const auto Failed = Asset::ApplyAssetPackageMigrationV8(std::move(Plan), {
		.ShouldFail = [](Asset::EAssetPackageMigrationApplyPhase Phase, size_t) {
			return Phase == Asset::EAssetPackageMigrationApplyPhase::PublishMain;
		}});
	EXPECT_EQ(Failed.Status, Asset::EAssetPackageMigrationApplyStatus::Failed);
	EXPECT_EQ(Failed.Plan.Packages.front().DiagnosticCode, "PublicationRolledBack");
	std::vector<std::byte> Restored;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(Restored, Fixture.MainPath));
	EXPECT_EQ(Restored, Fixture.V7);
	EXPECT_FALSE(std::filesystem::exists(Fixture.Root / "Fixture.dbulk"));
}

TEST(FPackageMigrationTests, StaleAndCancellationAdmissionAreTerminalAndReadOnly)
{
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	FMigrationFixture Fixture;
	Asset::FAssetPackageMigrationPlan Plan =
		Asset::PlanAssetPackageMigrationV8(std::span(&Fixture.Input, 1));
	Fixture.V7.push_back(std::byte{0});
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(Fixture.V7, Fixture.MainPath));
	const auto Stale = Asset::ApplyAssetPackageMigrationV8(std::move(Plan));
	EXPECT_EQ(Stale.Status, Asset::EAssetPackageMigrationApplyStatus::Failed);
	EXPECT_EQ(Stale.Plan.Packages.front().Status,
		Asset::EAssetPackageMigrationStatus::Stale);

	bool bCancel = true;
	const auto Cancelled = Asset::PlanAssetPackageMigrationV8(
		std::span(&Fixture.Input, 1), [&] { return bCancel; });
	EXPECT_EQ(Cancelled.Status, Asset::EAssetPackageMigrationPlanStatus::Cancelled);
	EXPECT_TRUE(Cancelled.Packages.empty());
}
