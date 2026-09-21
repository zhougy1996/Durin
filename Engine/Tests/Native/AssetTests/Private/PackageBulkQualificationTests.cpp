#include "BulkPackageTestSupport.h"
#include "Asset/Asset.h"
#include "Asset/PackageSerialization.h"
#include "Asset/RegistryOperations.h"
#include "Asset/PackageResource.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeAssetTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/Task.h"
#include <gtest/gtest.h>
#include <chrono>
#include <iostream>

namespace
{
	auto InitializeBulkQualification() -> void
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		(void)DBulkPackageAssetForTest::StaticClass();
		Durin::ShutdownAssetManager();
		Durin::CollectGarbage();
		const auto Root = Durin::Testing::CreateTestFixtureDirectory("BulkQualification");
		Durin::Testing::RegisterMountPointForTests("/TestAssets/", Root.generic_string() + "/");
		Durin::InitializeAssetManager();
		if (!Durin::RefreshAssetRegistry(Durin::EAssetRegistryScanMode::FullValidation))
			throw std::runtime_error("Failed to initialize the bulk qualification registry.");
	}

	class FPackageBulkQualificationTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			bOwnsScheduler = !Durin::IsTaskSchedulerRunning();
			if (bOwnsScheduler) ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
			InitializeBulkQualification();
		}

		auto TearDown() -> void override
		{
			Durin::ShutdownAssetManager();
			Durin::CollectGarbage();
			if (bOwnsScheduler) Durin::ShutdownTaskScheduler(true);
		}

	private:
		bool bOwnsScheduler = false;
	};

}

TEST_F(FPackageBulkQualificationTests, FieldBulkClosureMeetsBoundedLooseFixtureBudgets)
{
	using namespace Durin;
	constexpr uint64 PayloadBytes = 4ull * 1024ull * 1024ull;
	constexpr double MetadataLoadBudgetMilliseconds = 500.0;
	constexpr double FirstAccessBudgetMilliseconds = 500.0;
	constexpr double SaveBudgetMilliseconds = 2000.0;

	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TestAssets/FieldBulkQualification", Path));
	DBulkPackageAssetForTest* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	Durin::FByteBuffer Payload(static_cast<size_t>(PayloadBytes));
	for (size_t Index = 0; Index < Payload.size(); ++Index)
		Payload[Index] = static_cast<std::byte>((Index * 131u + 17u) & 0xffu);
	ASSERT_TRUE(Asset->Payload.UpdatePayload(Payload));
	const FAssetWriteResult SaveResult = SavePackage(Asset->GetPackage());
	ASSERT_TRUE(SaveResult) << SaveResult.Message;
	ASSERT_TRUE(UnloadPackage(Path));

	const FAssetCatalogEntry Entry = FindAssetExact(Path);
	ASSERT_TRUE(Entry);
	std::filesystem::path SegmentPath = Entry->PhysicalPath;
	SegmentPath.replace_extension(".dbulk");
	ASSERT_TRUE(std::filesystem::is_regular_file(SegmentPath));
	EXPECT_EQ(std::filesystem::file_size(SegmentPath), PayloadBytes);
	EXPECT_EQ(GetPackageResourceManager().GetRegisteredPackageCount(), 0u);

	const auto MetadataStart = std::chrono::steady_clock::now();
	DObject* LoadedObject = nullptr;
	{
		auto LoadedValue = LoadObject<DObject>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
		LoadedObject = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	const double MetadataMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - MetadataStart).count();
	auto* Loaded = Cast<DBulkPackageAssetForTest>(LoadedObject);
	ASSERT_NE(Loaded, nullptr);
	EXPECT_LT(MetadataMilliseconds, MetadataLoadBudgetMilliseconds);
	EXPECT_FALSE(Loaded->Payload.IsMemoryResident());
	EXPECT_EQ(GetPackageResourceManager().GetRegisteredPackageCount(), 1u);
	const FPackageResourceHandle Resource =
		GetPackageResourceManager().FindPackage(Path.ToString());
	ASSERT_NE(Resource, nullptr);
	const FPackageResourceReadStats MetadataReadStats = Resource->GetReadStats();
	EXPECT_GT(MetadataReadStats.ValidationReadCount, 0u);
	EXPECT_EQ(MetadataReadStats.ValidationBytesRead, PayloadBytes);
	EXPECT_LE(MetadataReadStats.PeakValidationScratchBytes, 64u * 1024u);
	EXPECT_EQ(MetadataReadStats.RequestCount, 0u);
	EXPECT_EQ(MetadataReadStats.RequestedBytes, 0u);

	const auto AccessStart = std::chrono::steady_clock::now();
	const FPackageResourceReadResult LoadedPayload = Loaded->Payload.GetPayload().Wait();
	const double AccessMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - AccessStart).count();
	ASSERT_TRUE(LoadedPayload) << Durin::FormatPackageResourceReadError(LoadedPayload);
	EXPECT_LT(AccessMilliseconds, FirstAccessBudgetMilliseconds);
	EXPECT_EQ(LoadedPayload.Buffer.GetSize(), PayloadBytes);
	EXPECT_TRUE(std::ranges::equal(LoadedPayload.Buffer.GetBytes(), Payload));
	EXPECT_FALSE(Loaded->Payload.IsMemoryResident());
	const FPackageResourceReadStats AccessReadStats = Resource->GetReadStats();
	EXPECT_EQ(AccessReadStats.ValidationBytesRead, PayloadBytes);
	EXPECT_EQ(AccessReadStats.RequestCount, 1u);
	EXPECT_EQ(AccessReadStats.RequestedBytes, PayloadBytes);

	const auto SaveStart = std::chrono::steady_clock::now();
	ASSERT_TRUE(SavePackage(Loaded->GetPackage()));
	const double SaveMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - SaveStart).count();
	EXPECT_LT(SaveMilliseconds, SaveBudgetMilliseconds);
	EXPECT_EQ(std::filesystem::file_size(SegmentPath), PayloadBytes);

	std::cout << "FieldBulkQualification payload_bytes=" << PayloadBytes
		<< " segment_bytes=" << std::filesystem::file_size(SegmentPath)
		<< " resident_field_bytes=0"
		<< " validation_bytes=" << MetadataReadStats.ValidationBytesRead
		<< " validation_peak_scratch=" << MetadataReadStats.PeakValidationScratchBytes
		<< " range_request_bytes=" << AccessReadStats.RequestedBytes
		<< " metadata_load_ms=" << MetadataMilliseconds
		<< " first_access_ms=" << AccessMilliseconds
		<< " save_ms=" << SaveMilliseconds << '\n';
	EXPECT_TRUE(UnloadPackage(Path));
	EXPECT_EQ(GetPackageResourceManager().GetRegisteredPackageCount(), 0u);
}
