#include "AssetRegistry/PackageHeader.h"
#include "AssetRegistry/References.h"
#include "AssetRegistry/Scan.h"
#include "DObject/PackageFormat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/BinaryFormat.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	namespace Asset = Durin::Asset;
	namespace Package = Durin::ObjectPackage;

	auto MakeRegistryFixture() -> Package::FLinkerTables
	{
		Package::FLinkerTables Linker;
		Linker.Summary.PackageName = "/Game/RegistryFixture";
		Linker.Summary.AssetClass = "Example::RegistryAsset";
		Linker.Summary.HardPackageReferences = {"/Game/HardB", "/Game/HardA"};
		Linker.Summary.SoftPackageReferences = {"/Game/SoftB", "/Game/SoftA"};
		Linker.Summary.SearchableNames = {"Tag.Z", "Tag.A"};
		Package::FPackageIndex::TryExport(0, Linker.Summary.MainExport);
		Linker.Exports = {{.ObjectName = "RegistryFixture", .ClassName = "Example::RegistryAsset"}};
		return Linker;
	}

	auto Path(std::string_view Value) -> Durin::FAssetPath
	{
		Durin::FAssetPath Result;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate(Value, Result));
		return Result;
	}
}

TEST(FPackageRegistryContractTests, V8FrontMatterProjectsPackageLevelMetadata)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Game/", ".");
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(MakeRegistryFixture(), Main, Bulk));
	uint64 HeaderBytes = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt<uint64>(Main, 32, HeaderBytes));
	Durin::FAssetPath PackagePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/RegistryFixture", PackagePath));
	Asset::FAssetPackageHeader Header;
	const Asset::FAssetResult Result = Asset::ReadAssetPackageHeaderBytes(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), Bulk.size(),
		PackagePath, Header);
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Header.FormatVersion, Asset::AssetPackageV8FormatVersion);
	EXPECT_EQ(Header.AssetClassName, "Example::RegistryAsset");
	EXPECT_EQ(Header.EntryKind, Asset::EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Header.ObjectCount, 1u);
	EXPECT_EQ(Header.Dependencies, (std::vector<Durin::FAssetPath>{
		Path("/Game/HardA"), Path("/Game/HardB")}));
	EXPECT_EQ(Header.SoftDependencies, (std::vector<Durin::FAssetPath>{
		Path("/Game/SoftA"), Path("/Game/SoftB")}));
	EXPECT_EQ(Header.SearchableNames, (std::vector<std::string>{"Tag.A", "Tag.Z"}));
	EXPECT_EQ(Header.BulkSegmentExtent, 0u);
	EXPECT_TRUE(Header.BulkSegmentDigest.IsZero());
	EXPECT_EQ(Header.BytesRead, HeaderBytes);
}

TEST(FPackageRegistryContractTests, V8ProjectionRequiresIdentityAndExactBulkExtent)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Game/", ".");
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(MakeRegistryFixture(), Main, Bulk));
	uint64 HeaderBytes = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt<uint64>(Main, 32, HeaderBytes));
	Durin::FAssetPath Correct;
	Durin::FAssetPath Wrong;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/RegistryFixture", Correct));
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/Wrong", Wrong));
	Asset::FAssetPackageHeader Header;
	EXPECT_FALSE(Asset::ReadAssetPackageHeaderBytes(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), 1, Correct, Header));
	EXPECT_FALSE(Asset::ReadAssetPackageHeaderBytes(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), 0, Wrong, Header));
}

TEST(FPackageRegistryContractTests, RefreshUsesOnlyFrontMatterAndOnePackageMetadataCache)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	const std::filesystem::path WorkRoot =
		Durin::Testing::GetTestWorkDirectory() / "PackageLevelRegistry";
	const std::filesystem::path ContentRoot = WorkRoot / "Content";
	const std::filesystem::path CacheRoot = WorkRoot / "DerivedDataCache";
	Durin::Testing::RemoveTestWorkDirectory(WorkRoot);
	std::filesystem::create_directories(ContentRoot);

	Package::FLinkerTables Linker = MakeRegistryFixture();
	Linker.Summary.PackageName = "/P3/Owner";
	Linker.Summary.HardPackageReferences = {"/P3/HardB", "/P3/HardA"};
	Linker.Summary.SoftPackageReferences = {"/P3/SoftB", "/P3/SoftA"};
	std::vector<std::byte> Main;
	std::vector<std::byte> Bulk;
	ASSERT_TRUE(Package::WritePackageV8(Linker, Main, Bulk));
	uint64 HeaderBytes = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt<uint64>(Main, 32, HeaderBytes));
	ASSERT_LT(HeaderBytes, Main.size());
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		Main, ContentRoot / "Owner.dasset"));
	if (!Bulk.empty()) ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		Bulk, ContentRoot / "Owner.dbulk"));

	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests(
		"/P3/", ContentRoot.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const Asset::FAssetCatalogRefreshResult Cold = Asset::RefreshAssetRegistry(
		Asset::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Cold) << (Cold.Errors.empty() ? "" : Cold.Errors.front().Message);
	EXPECT_EQ(Cold.CatalogStats.Enumerated, 1u);
	EXPECT_EQ(Cold.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(Cold.CatalogStats.HeaderFileBytesRead, HeaderBytes);
	EXPECT_LT(Cold.CatalogStats.HeaderFileBytesRead, Main.size());
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CacheRoot / "AssetRegistry" / "Registry.bin"));
	EXPECT_FALSE(std::filesystem::exists(
		CacheRoot / "AssetRegistry" / "References.bin"));

	const Durin::FAssetPath Owner = Path("/P3/Owner");
	const Asset::FAssetCatalogEntry Data = Asset::FindAssetExact(Owner);
	ASSERT_TRUE(Data);
	EXPECT_EQ(Data->Dependencies, (std::vector<Durin::FAssetPath>{
		Path("/P3/HardA"), Path("/P3/HardB")}));
	EXPECT_EQ(Data->SoftDependencies, (std::vector<Durin::FAssetPath>{
		Path("/P3/SoftA"), Path("/P3/SoftB")}));
	EXPECT_EQ(Data->SearchableNames,
		(std::vector<std::string>{"Tag.A", "Tag.Z"}));
	const Asset::FAssetReferenceIndex ColdIndex =
		Asset::CaptureAssetReferenceIndex();
	ASSERT_TRUE(ColdIndex.IsComplete());
	EXPECT_EQ(ColdIndex.GetEdges().size(), 4u);
	const std::vector<Asset::FAssetPackageReferenceEdge> ColdEdges(
		ColdIndex.GetEdges().begin(), ColdIndex.GetEdges().end());
	EXPECT_EQ(ColdIndex.FindTargets(Owner), (std::vector<Durin::FAssetPath>{
		Path("/P3/HardA"), Path("/P3/HardB"),
		Path("/P3/SoftA"), Path("/P3/SoftB")}));

	const Asset::FAssetCatalogRefreshResult Warm = Asset::RefreshAssetRegistry();
	ASSERT_TRUE(Warm);
	EXPECT_EQ(Warm.CatalogStats.Reused, 1u);
	EXPECT_EQ(Warm.CatalogStats.Reparsed, 0u);
	EXPECT_EQ(Warm.CatalogStats.HeaderReadAttempts, 0u);
	const Asset::FAssetReferenceIndex WarmIndex = Asset::CaptureAssetReferenceIndex();
	const auto WarmEdges = WarmIndex.GetEdges();
	EXPECT_EQ((std::vector<Asset::FAssetPackageReferenceEdge>(
		WarmEdges.begin(), WarmEdges.end())), ColdEdges);

	const Asset::FAssetCatalogRefreshResult Full = Asset::RefreshAssetRegistry(
		Asset::EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Full);
	EXPECT_EQ(Full.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(Full.CatalogStats.HeaderFileBytesRead, HeaderBytes);
	const Asset::FAssetReferenceIndex FullIndex = Asset::CaptureAssetReferenceIndex();
	const auto FullEdges = FullIndex.GetEdges();
	EXPECT_EQ((std::vector<Asset::FAssetPackageReferenceEdge>(
		FullEdges.begin(), FullEdges.end())), ColdEdges);

	const std::filesystem::path CacheFile =
		CacheRoot / "AssetRegistry" / "Registry.bin";
	std::vector<std::byte> CorruptCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(CorruptCache, CacheFile));
	ASSERT_GE(CorruptCache.size(), sizeof(uint32) * 2);
	const uint32 UnknownSchema = 99;
	std::memcpy(CorruptCache.data() + sizeof(uint32), &UnknownSchema,
		sizeof(UnknownSchema));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(CorruptCache, CacheFile));
	const Asset::FAssetCatalogRefreshResult Recovered = Asset::RefreshAssetRegistry();
	ASSERT_TRUE(Recovered);
	EXPECT_EQ(Recovered.CatalogStats.Reparsed, 1u);
	EXPECT_FALSE(Recovered.CatalogCacheWarning.empty());
	const Asset::FAssetReferenceIndex RecoveredIndex =
		Asset::CaptureAssetReferenceIndex();
	EXPECT_EQ((std::vector<Asset::FAssetPackageReferenceEdge>(
		RecoveredIndex.GetEdges().begin(), RecoveredIndex.GetEdges().end())), ColdEdges);
}
