#include "AssetRegistry/PackageHeader.h"
#include "AssetRegistry/References.h"
#include "AssetRegistry/Publication.h"
#include "AssetRegistry/Scan.h"
#include "DObject/PackageFormat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	namespace Package = Durin::ObjectPackage;

	auto MakeRegistryFixture(
		std::string_view PackageName = "/Game/RegistryFixture",
		std::initializer_list<std::string_view> Hard = {"/Game/HardB", "/Game/HardA"},
		std::initializer_list<std::string_view> Soft = {"/Game/SoftB", "/Game/SoftA"})
		-> Package::FLinkerTables
	{
		Package::FLinkerTables Linker;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(PackageName, Linker.Summary.PackagePath));
		Linker.Summary.SearchableNames = {"Tag.Z", "Tag.A"};
		Package::FPackageIndex MainExport;
		Package::FPackageIndex::TryExport(0, MainExport);
		Linker.Exports = {{.ObjectName = "RegistryFixture", .ClassName = "Example::RegistryAsset"}};
		Durin::FTopLevelAssetPath AssetPath;
		EXPECT_TRUE(Durin::FTopLevelAssetPath::TryCreate(
			Linker.Summary.PackagePath, "RegistryFixture", AssetPath));
		Linker.Summary.TopLevelAssets.push_back({
			.Export = MainExport,
			.AssetPath = AssetPath,
			.ClassName = "Example::RegistryAsset"});
		for (std::string_view Value : Hard)
		{
			Durin::FPackagePath Dependency;
			EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Dependency));
			Linker.Summary.HardPackageDependencies.push_back(std::move(Dependency));
		}
		for (std::string_view Value : Soft)
		{
			Durin::FPackagePath Dependency;
			EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Dependency));
			Linker.Summary.SoftPackageDependencies.push_back(std::move(Dependency));
		}
		return Linker;
	}

	auto Path(std::string_view Value) -> Durin::FPackagePath
	{
		Durin::FPackagePath Result;
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Value, Result));
		return Result;
	}
}

TEST(FPackageRegistryContractTests, V9FrontMatterProjectsPackageAndTopLevelAssetMetadata)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Game/", ".");
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	ASSERT_TRUE(Package::WritePackageV9(MakeRegistryFixture(), Main, Bulk));
	uint64 HeaderBytes = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt<uint64>(Main, 32, HeaderBytes));
	Durin::FPackagePath PackagePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/RegistryFixture", PackagePath));
	FAssetPackageHeader Header;
	const FAssetRegistryResult Result = ReadAssetPackageHeaderBytes(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), Bulk.size(),
		PackagePath, Header);
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Header.FormatVersion, Durin::ObjectPackage::DastV9FormatVersion);
	ASSERT_EQ(Header.TopLevelAssets.size(), 1u);
	EXPECT_EQ(Header.TopLevelAssets.front().AssetPath.ToString(),
		"/Game/RegistryFixture.RegistryFixture");
	EXPECT_EQ(Header.TopLevelAssets.front().AssetClassName,
		"Example::RegistryAsset");
	EXPECT_EQ(Header.AssetClassName, "Example::RegistryAsset");
	EXPECT_EQ(Header.EntryKind, EAssetRegistryEntryKind::Asset);
	EXPECT_EQ(Header.ObjectCount, 1u);
	EXPECT_EQ(Header.Dependencies, (std::vector<Durin::FPackagePath>{
		Path("/Game/HardA"), Path("/Game/HardB")}));
	EXPECT_EQ(Header.SoftDependencies, (std::vector<Durin::FPackagePath>{
		Path("/Game/SoftA"), Path("/Game/SoftB")}));
	EXPECT_EQ(Header.SearchableNames, (std::vector<std::string>{"Tag.A", "Tag.Z"}));
	EXPECT_EQ(Header.BulkSegmentExtent, 0u);
	EXPECT_TRUE(Header.BulkSegmentDigest.IsZero());
	EXPECT_EQ(Header.BytesRead, HeaderBytes);
}

TEST(FPackageRegistryContractTests, V9ProjectionRequiresIdentityAndExactBulkExtent)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Game/", ".");
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	ASSERT_TRUE(Package::WritePackageV9(MakeRegistryFixture(), Main, Bulk));
	uint64 HeaderBytes = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt<uint64>(Main, 32, HeaderBytes));
	Durin::FPackagePath Correct;
	Durin::FPackagePath Wrong;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/RegistryFixture", Correct));
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/Wrong", Wrong));
	FAssetPackageHeader Header;
	EXPECT_FALSE(ReadAssetPackageHeaderBytes(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), 1, Correct, Header));
	EXPECT_FALSE(ReadAssetPackageHeaderBytes(
		std::span(Main).first(static_cast<size_t>(HeaderBytes)), Main.size(), 0, Wrong, Header));
}

TEST(FPackageRegistryContractTests, ProductionProjectionRejectsRetiredV7)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Game/", ".");
	std::array<std::byte, Durin::BinaryEnvelopePreambleBytes> Retired{};
	const Durin::FBinaryEnvelopePreamble Preamble{
		.FormatId = Durin::ObjectPackage::DastFormatId,
		.FormatVersion = 7,
		.HeaderBytes = Retired.size(),
		.FileBytes = Retired.size()};
	ASSERT_TRUE(Durin::EncodeBinaryEnvelopePreamble(Preamble, Retired));
	ASSERT_TRUE(Durin::FinalizeBinaryEnvelopeHeader(Retired, Retired.size(),
		{16ull * 1024ull * 1024ull, 1024ull * 1024ull * 1024ull}));
	FAssetPackageHeader Header;
	const FAssetRegistryResult Result = ReadAssetPackageHeaderBytes(
		Retired, Retired.size(), 0, Path("/Game/Retired"), Header);
	EXPECT_EQ(Result.Error, EAssetRegistryError::UnsupportedVersion);
	EXPECT_EQ(Header.FormatVersion, 0u);
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
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests(
		"/P3/", ContentRoot.generic_string() + "/");

	Package::FLinkerTables Linker = MakeRegistryFixture(
		"/P3/Owner", {"/P3/HardB", "/P3/HardA"},
		{"/P3/SoftB", "/P3/SoftA"});
	Package::FPackageIndex SecondaryExport;
	ASSERT_TRUE(Package::FPackageIndex::TryExport(1, SecondaryExport));
	Durin::FTopLevelAssetPath SecondaryPath;
	ASSERT_TRUE(Durin::FTopLevelAssetPath::TryCreate(
		Linker.Summary.PackagePath, "Secondary", SecondaryPath));
	Linker.Exports.push_back({
		.ObjectName = "Secondary", .ClassName = "Example::SecondaryAsset"});
	Linker.Summary.TopLevelAssets.push_back({
		.Export = SecondaryExport,
		.AssetPath = SecondaryPath,
		.ClassName = "Example::SecondaryAsset"});
	Durin::FByteBuffer Main;
	Durin::FByteBuffer Bulk;
	ASSERT_TRUE(Package::WritePackageV9(Linker, Main, Bulk));
	uint64 HeaderBytes = 0;
	ASSERT_TRUE(Durin::ReadLittleEndianAt<uint64>(Main, 32, HeaderBytes));
	ASSERT_LT(HeaderBytes, Main.size());
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		Main, ContentRoot / "Owner.dasset"));
	if (!Bulk.empty()) ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		Bulk, ContentRoot / "Owner.dbulk"));

	Durin::FPaths::SetDerivedDataCacheDirForTests(CacheRoot.generic_string());
	const FAssetCatalogRefreshResult Cold = RefreshAssetRegistry(
		EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Cold) << (Cold.Errors.empty() ? "" : Cold.Errors.front().Message);
	EXPECT_EQ(Cold.CatalogStats.Enumerated, 1u);
	EXPECT_EQ(Cold.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(Cold.CatalogStats.HeaderFileBytesRead, HeaderBytes);
	EXPECT_LT(Cold.CatalogStats.HeaderFileBytesRead, Main.size());
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CacheRoot / "AssetRegistry" / "Registry.bin"));
	EXPECT_FALSE(std::filesystem::exists(
		CacheRoot / "AssetRegistry" / "References.bin"));

	const Durin::FPackagePath Owner = Path("/P3/Owner");
	const FAssetCatalogEntry Data = FindAssetExact(Owner);
	ASSERT_TRUE(Data);
	ASSERT_EQ(Data->TopLevelAssets.size(), 2u);
	EXPECT_EQ(Data->TopLevelAssets.front().AssetPath.ToString(),
		"/P3/Owner.RegistryFixture");
	EXPECT_EQ(Data->TopLevelAssets.back().AssetPath, SecondaryPath);
	const FTopLevelAssetCatalogEntry Secondary =
		FindTopLevelAssetExact(SecondaryPath);
	ASSERT_TRUE(Secondary);
	EXPECT_EQ(Secondary->AssetClassName, "Example::SecondaryAsset");
	ASSERT_TRUE(Secondary.Package.has_value());
	EXPECT_EQ(Secondary.Package->PackagePath, Owner);
	EXPECT_EQ(Data->Dependencies, (std::vector<Durin::FPackagePath>{
		Path("/P3/HardA"), Path("/P3/HardB")}));
	EXPECT_EQ(Data->SoftDependencies, (std::vector<Durin::FPackagePath>{
		Path("/P3/SoftA"), Path("/P3/SoftB")}));
	EXPECT_EQ(Data->SearchableNames,
		(std::vector<std::string>{"Tag.A", "Tag.Z"}));
	const FAssetReferenceIndex ColdIndex =
		CaptureAssetReferenceIndex();
	ASSERT_TRUE(ColdIndex.IsComplete());
	EXPECT_EQ(ColdIndex.GetEdges().size(), 4u);
	const std::vector<FAssetPackageReferenceEdge> ColdEdges(
		ColdIndex.GetEdges().begin(), ColdIndex.GetEdges().end());
	EXPECT_EQ(ColdIndex.FindTargets(Owner), (std::vector<Durin::FPackagePath>{
		Path("/P3/HardA"), Path("/P3/HardB"),
		Path("/P3/SoftA"), Path("/P3/SoftB")}));

	const FAssetCatalogRefreshResult Warm = RefreshAssetRegistry();
	ASSERT_TRUE(Warm);
	EXPECT_EQ(Warm.CatalogStats.Reused, 1u);
	EXPECT_EQ(Warm.CatalogStats.Reparsed, 0u);
	EXPECT_EQ(Warm.CatalogStats.HeaderReadAttempts, 0u);
	const FAssetReferenceIndex WarmIndex = CaptureAssetReferenceIndex();
	const auto WarmEdges = WarmIndex.GetEdges();
	EXPECT_EQ((std::vector<FAssetPackageReferenceEdge>(
		WarmEdges.begin(), WarmEdges.end())), ColdEdges);

	const FAssetCatalogRefreshResult Full = RefreshAssetRegistry(
		EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Full);
	EXPECT_EQ(Full.CatalogStats.Reparsed, 1u);
	EXPECT_EQ(Full.CatalogStats.HeaderFileBytesRead, HeaderBytes);
	const FAssetReferenceIndex FullIndex = CaptureAssetReferenceIndex();
	const auto FullEdges = FullIndex.GetEdges();
	EXPECT_EQ((std::vector<FAssetPackageReferenceEdge>(
		FullEdges.begin(), FullEdges.end())), ColdEdges);

	const std::filesystem::path CacheFile =
		CacheRoot / "AssetRegistry" / "Registry.bin";
	Durin::FByteBuffer CorruptCache;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(CorruptCache, CacheFile));
	ASSERT_GE(CorruptCache.size(), sizeof(uint32) * 2);
	const uint32 UnknownSchema = 99;
	std::memcpy(CorruptCache.data() + sizeof(uint32), &UnknownSchema,
		sizeof(UnknownSchema));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(CorruptCache, CacheFile));
	const FAssetCatalogRefreshResult Recovered = RefreshAssetRegistry();
	ASSERT_TRUE(Recovered);
	EXPECT_EQ(Recovered.CatalogStats.Reparsed, 1u);
	EXPECT_FALSE(Recovered.CatalogCacheWarning.empty());
	const FAssetReferenceIndex RecoveredIndex =
		CaptureAssetReferenceIndex();
	EXPECT_EQ((std::vector<FAssetPackageReferenceEdge>(
		RecoveredIndex.GetEdges().begin(), RecoveredIndex.GetEdges().end())), ColdEdges);
}

TEST(FPackageRegistryContractTests, MultiAssetRedirectsAreExactAcrossScansAndPublications)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	const auto WorkRoot = Durin::Testing::GetTestWorkDirectory() / "MultiAssetRedirects";
	Durin::Testing::RemoveTestWorkDirectory(WorkRoot);
	const auto ContentRoot = WorkRoot / "Content";
	std::filesystem::create_directories(ContentRoot);
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Multi/", ContentRoot.generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests((WorkRoot / "Cache").generic_string());
	auto ObjectPath = [](std::string_view Value) {
		FObjectPath Result;
		EXPECT_TRUE(FObjectPath::TryCreate(Value, Result));
		return Result;
	};
	auto Save = [&](const Package::FLinkerTables& Linker, std::string_view Name) {
		FByteBuffer Main, Bulk;
		EXPECT_TRUE(Package::WritePackageV9(Linker, Main, Bulk));
		EXPECT_TRUE(FFileHelper::SaveArrayToFile(Main,
			ContentRoot / (std::string(Name) + ".dasset")));
	};
	for (const std::string Name : {"TargetA", "TargetB"})
	{
		auto Linker = MakeRegistryFixture("/Multi/" + Name, {}, {});
		Linker.Exports.front().ClassName = "Durin::DObject";
		Linker.Summary.TopLevelAssets.front().ClassName = "Durin::DObject";
		Save(Linker, Name);
	}
	for (const std::string Name : {"Aliases", "MixedFirst", "MixedLast"})
	{
		auto Linker = MakeRegistryFixture("/Multi/" + Name,
			{"/Multi/TargetA", "/Multi/TargetB"}, {});
		Linker.Exports.clear();
		Linker.Summary.TopLevelAssets.clear();
		auto Add = [&](std::string_view AssetName, std::string_view Destination) {
			Package::FPackageIndex Export;
			EXPECT_TRUE(Package::FPackageIndex::TryExport(
				static_cast<uint32>(Linker.Exports.size()), Export));
			FTopLevelAssetPath AssetPath;
			EXPECT_TRUE(FTopLevelAssetPath::TryCreate(Linker.Summary.PackagePath,
				AssetName, AssetPath));
			const std::string ClassName = Destination.empty()
				? "Durin::DObject" : "Durin::DAssetRedirector";
			Linker.Exports.push_back({.ObjectName = std::string(AssetName), .ClassName = ClassName});
			Linker.Summary.TopLevelAssets.push_back({.Export = Export,
				.AssetPath = AssetPath, .ClassName = ClassName,
				.RedirectDestination = Destination.empty() ? FObjectPath{} : ObjectPath(Destination)});
		};
		Add("MAlias", "/Multi/TargetA.RegistryFixture");
		Add("NAlias", "/Multi/TargetB.RegistryFixture");
		if (Name != "Aliases") Add(Name == "MixedFirst" ? "AOrdinary" : "ZOrdinary", {});
		Save(Linker, Name);
	}
	const auto Cold = RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Cold) << (Cold.Errors.empty() ? "" : Cold.Errors.front().Message);
	EXPECT_EQ(Cold.CatalogStats.Redirectors, 6u);
	const auto ColdPublication = CaptureAssetRegistryPublication();
	for (const std::string Name : {"Aliases", "MixedFirst", "MixedLast"})
	{
		const auto Source = Path("/Multi/" + Name);
		const auto Data = FindAssetExact(Source);
		ASSERT_TRUE(Data);
		EXPECT_TRUE(Data->AssetClassName.empty());
		EXPECT_FALSE(Data->RedirectDestination.IsValid());
		const auto First = ResolveAssetObjectPath(ObjectPath("/Multi/" + Name + ".MAlias"));
		ASSERT_TRUE(First);
		EXPECT_EQ(First.FinalPath, ObjectPath("/Multi/TargetA.RegistryFixture"));
		const auto Second = ResolveAssetObjectPath(ObjectPath("/Multi/" + Name + ".NAlias"));
		ASSERT_TRUE(Second);
		EXPECT_EQ(Second.FinalPath, ObjectPath("/Multi/TargetB.RegistryFixture"));
		const auto Index = CaptureAssetReferenceIndex();
		for (const auto& Target : {Path("/Multi/TargetA"), Path("/Multi/TargetB")})
		{
			const auto Edges = Index.FindReferencers(Target);
			EXPECT_TRUE(std::ranges::any_of(Edges, [&](const auto& Edge) {
				return Edge.SourcePackage == Source && Edge.Kind == EAssetReferenceKind::Redirect;
			}));
			EXPECT_EQ(std::ranges::any_of(Edges, [&](const auto& Edge) {
				return Edge.SourcePackage == Source && Edge.Kind == EAssetReferenceKind::HardObject;
			}), Name != "Aliases");
		}
	}
	EXPECT_EQ(FindRedirectorsTo(Path("/Multi/TargetB")).size(), 3u);
	const auto Warm = RefreshAssetRegistry();
	ASSERT_TRUE(Warm);
	EXPECT_EQ(Warm.CatalogStats.Reused, 5u);
	EXPECT_EQ(CaptureAssetRegistryPublication().Assets, ColdPublication.Assets);
	EXPECT_EQ(CaptureAssetRegistryPublication().ReferenceEdges, ColdPublication.ReferenceEdges);
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	EXPECT_EQ(CaptureAssetRegistryPublication().ReferenceEdges, ColdPublication.ReferenceEdges);

	FAssetData Reordered = *FindAssetExact(Path("/Multi/MixedFirst"));
	std::ranges::reverse(Reordered.TopLevelAssets);
	FAssetRegistryDelta Delta{.ExpectedRevision = GetAssetCatalogRevision(), .Replaces = {Reordered}};
	const auto Published = PublishAssetRegistryDelta(std::move(Delta));
	ASSERT_TRUE(Published) << Published.Message;
	EXPECT_EQ(CaptureAssetRegistryPublication().ReferenceEdges, ColdPublication.ReferenceEdges);
	const uint64 Revision = GetAssetCatalogRevision();
	Reordered.TopLevelAssets.back().AssetClassName = "Durin::DAssetRedirector";
	const auto Rejected = PublishAssetRegistryDelta({.ExpectedRevision = Revision, .Replaces = {Reordered}});
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(GetAssetCatalogRevision(), Revision);
}

TEST(FPackageRegistryContractTests, ProjectionFenceBlocksEveryRedirectHop)
{
	Durin::Testing::InitializeDObjectSystemForTests();
	const auto WorkRoot = Durin::Testing::GetTestWorkDirectory() / "ProjectionFences";
	Durin::Testing::RemoveTestWorkDirectory(WorkRoot);
	std::filesystem::create_directories(WorkRoot / "Content");
	Durin::Testing::FScopedMountRegistryFixture Mounts;
	Durin::Testing::RegisterMountPointForTests("/Fences/",
		(WorkRoot / "Content").generic_string() + "/");
	Durin::FPaths::SetDerivedDataCacheDirForTests((WorkRoot / "Cache").generic_string());
	auto ObjectPath = [](std::string_view Name) {
		FObjectPath Result;
		EXPECT_TRUE(FObjectPath::TryCreate(
			std::string(Name) + ".RegistryFixture", Result));
		return Result;
	};
	const std::string Names[] = {"/Fences/Source", "/Fences/Middle", "/Fences/Target"};
	for (size_t Index = 0; Index < 3; ++Index)
	{
		auto Linker = MakeRegistryFixture(Names[Index], {}, {});
		const bool bRedirect = Index < 2;
		const std::string ClassName = bRedirect ? "Durin::DAssetRedirector" : "Durin::DObject";
		Linker.Exports.front().ClassName = ClassName;
		Linker.Summary.TopLevelAssets.front().ClassName = ClassName;
		if (bRedirect)
		{
			Linker.Summary.HardPackageDependencies.push_back(Path(Names[Index + 1]));
			Linker.Summary.TopLevelAssets.front().RedirectDestination = ObjectPath(Names[Index + 1]);
		}
		FByteBuffer Main, Bulk;
		ASSERT_TRUE(Package::WritePackageV9(Linker, Main, Bulk));
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Main,
			WorkRoot / "Content" / (std::filesystem::path(Names[Index]).filename().string() + ".dasset")));
	}
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	const auto Snapshot = CaptureAssetRegistrySnapshot();
	ASSERT_TRUE(ResolveAssetObjectPath(ObjectPath(Names[0])));
	for (size_t Index = 0; Index < 3; ++Index)
	{
		const FPackagePath Fenced[] = {Path(Names[Index])};
		FenceAssetRegistryProjection(Fenced);
		const auto Exact = ResolveAssetObjectPath(ObjectPath(Names[0]));
		EXPECT_EQ(Exact.State, EAssetPathResolveState::ProjectionPending);
		EXPECT_EQ(Exact.FinalPath, ObjectPath(Names[Index]));
		EXPECT_EQ(Exact.RedirectChain.size(), Index);
		EXPECT_FALSE(Exact.FinalPackageData.has_value());
		const auto PackageResult = ResolveAssetPath(Path(Names[0]));
		EXPECT_EQ(PackageResult.State, EAssetPathResolveState::ProjectionPending);
		EXPECT_EQ(PackageResult.FinalPath, Fenced[0]);
		EXPECT_EQ(Snapshot.ResolveAssetPath(Path(Names[0])).State,
			EAssetPathResolveState::ProjectionPending);
		EXPECT_EQ(ResolveAssetObjectPath(ObjectPath(Names[Index])).State,
			EAssetPathResolveState::ProjectionPending);
		ClearAssetRegistryProjectionFence(Fenced);
		EXPECT_TRUE(ResolveAssetPath(Path(Names[0])));
		EXPECT_TRUE(ResolveAssetObjectPath(ObjectPath(Names[0])));
	}
}
