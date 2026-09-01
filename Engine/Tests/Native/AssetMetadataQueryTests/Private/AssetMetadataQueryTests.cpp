#include "AssetRegistry/Catalog.h"
#include "DObject/PackageFormat.h"
#include "AssetRegistry/PackageHeader.h"
#include "AssetRegistry/PackageTypes.h"
#include "AssetRegistry/Publication.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	auto MakeAssetData(const Durin::FPackagePath& Path,
		std::vector<Durin::FPackagePath> Hard = {},
		std::vector<Durin::FPackagePath> Soft = {}) -> FAssetData
	{
		return {.PackagePath = Path,
			.AssetClassName = "Example::MetadataAsset",
			.FormatVersion = Durin::ObjectPackage::DastV9FormatVersion,
			.Dependencies = std::move(Hard),
			.SoftDependencies = std::move(Soft),
			.ObjectCount = 1,
			.FileSize = 128,
			.LastWriteTimeTicks = 42};
	}

		auto RebuildPackageProjection(FAssetRegistryPublication& Publication) -> void
	{
		Publication.ReferenceEdges.clear();
		Publication.ReferenceFingerprints.clear();
		for (const auto& [Path, Data] : Publication.Assets)
		{
			const FAssetPackageFingerprint Fingerprint{
				.FileSize = Data.FileSize,
				.LastWriteTimeTicks = Data.LastWriteTimeTicks,
				.ReaderVersion = Data.FormatVersion};
			Publication.ReferenceFingerprints.emplace(Path, Fingerprint);
			auto Add = [&](EAssetReferenceKind Kind, const Durin::FPackagePath& Target) {
				Publication.ReferenceEdges.push_back({.SourcePackage = Path,
					.SourceFingerprint = Fingerprint, .Kind = Kind,
					.TargetPath = Target});
			};
			for (const Durin::FPackagePath& Target : Data.Dependencies)
				if (Data.EntryKind != EAssetRegistryEntryKind::Redirector
					|| Target != Data.RedirectDestination)
					Add(EAssetReferenceKind::HardObject, Target);
			for (const Durin::FPackagePath& Target : Data.SoftDependencies)
				Add(EAssetReferenceKind::SoftObject, Target);
			if (Data.EntryKind == EAssetRegistryEntryKind::Redirector)
				Add(EAssetReferenceKind::Redirect, Data.RedirectDestination);
		}
		std::ranges::sort(Publication.ReferenceEdges,
			[](const FAssetPackageReferenceEdge& Left,
				const FAssetPackageReferenceEdge& Right) {
				return std::tuple(Left.TargetPath.ToString(),
					Left.SourcePackage.GetView(), Left.Kind)
					< std::tuple(Right.TargetPath.ToString(),
						Right.SourcePackage.GetView(), Right.Kind);
			});
		Publication.ReferenceErrors.clear();
		Publication.bReferenceIndexComplete = true;
	}

	TEST(FAssetMetadataQueryTests, RegistryErrorsExposeStructuredDiagnostics)
	{
		const FAssetRegistryResult Result{
			EAssetRegistryError::StaleData, "The expected revision is stale."};
		const Durin::FDiagnostic Diagnostic = Result.GetDiagnostic();
		EXPECT_EQ(Diagnostic.Domain, "AssetRegistry");
		EXPECT_EQ(Diagnostic.Code, "StaleData");
		EXPECT_TRUE(Diagnostic.IsError());
		EXPECT_EQ(Diagnostic.Message, Result.Message);
	}

	TEST(FAssetMetadataQueryTests, SnapshotOwnsExactMetadataWithoutEngine)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests(
			"/MetadataTests/", "MetadataTests/");
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MetadataTests/Textures/Brick", Path));

		FAssetCatalogSnapshot Snapshot{
			.Revision = 17,
			.Assets = {{Path, FAssetData{
				.PackagePath = Path,
				.PhysicalPath = "Content/Textures/Brick.dasset",
				.AssetClassName = "Durin::DTexture2D",
				.FormatVersion = Durin::ObjectPackage::DastV9FormatVersion}}}};

		const FAssetData* Data = Snapshot.FindExact(Path);
		ASSERT_NE(Data, nullptr);
		EXPECT_EQ(Snapshot.Revision, 17u);
		EXPECT_EQ(Data->PackagePath, Path);
		EXPECT_EQ(Data->AssetClassName, "Durin::DTexture2D");
		EXPECT_EQ(Data->FormatVersion, Durin::ObjectPackage::DastV9FormatVersion);
	}

	TEST(FAssetMetadataQueryTests, OwnsCanonicalDastReaderIdentity)
	{
		EXPECT_EQ(Durin::ObjectPackage::DastV9FormatVersion, 9u);
		EXPECT_EQ(Durin::ObjectPackage::DastFormatName, "Durin.BinaryFormat.DAST");
		EXPECT_TRUE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(9));
		EXPECT_FALSE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(8));
		EXPECT_FALSE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(7));
		EXPECT_FALSE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(6));

		const FAssetPackageFingerprint Fingerprint{
			.FileSize = 128,
			.LastWriteTimeTicks = 42,
			.ReaderVersion = Durin::ObjectPackage::DastV9FormatVersion};
		EXPECT_EQ(Fingerprint.ReaderVersion, 9u);
	}

	TEST(FAssetMetadataQueryTests, PublishesWholeStateAgainstExpectedRevision)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests("/MetadataTests/", "MetadataTests/");
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MetadataTests/Published", Path));

		FAssetRegistryPublication First = CaptureAssetRegistryPublication();
		const uint64 Revision = First.ExpectedRevision;
		First.Assets.insert_or_assign(Path, FAssetData{
			.PackagePath = Path,
			.AssetClassName = "Durin::DTexture2D",
			.FormatVersion = Durin::ObjectPackage::DastV9FormatVersion,
			.ObjectCount = 1});
		RebuildPackageProjection(First);
		ASSERT_TRUE(PublishAssetRegistryPublication(std::move(First)));
		EXPECT_EQ(GetAssetCatalogRevision(), Revision + 1);
		ASSERT_TRUE(FindAssetExact(Path));

		FAssetRegistryPublication Stale = CaptureAssetRegistryPublication();
		Stale.ExpectedRevision = Revision;
		EXPECT_EQ(PublishAssetRegistryPublication(std::move(Stale)).Error,
			EAssetRegistryError::StaleData);
		EXPECT_TRUE(FindAssetExact(Path));

		FAssetRegistryPublication Incomplete = CaptureAssetRegistryPublication();
		Incomplete.bReferenceIndexComplete = false;
		EXPECT_EQ(PublishAssetRegistryPublication(std::move(Incomplete)).Error,
			EAssetRegistryError::StaleData);
	}

	TEST(FAssetMetadataQueryTests, ConcurrentExpectedRevisionPublishesAtMostOnce)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests("/MetadataTests/", "MetadataTests/");
		Durin::FPackagePath FirstPath, SecondPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MetadataTests/ConcurrentA", FirstPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MetadataTests/ConcurrentB", SecondPath));
		const FAssetRegistryPublication Base = CaptureAssetRegistryPublication();
		const uint64 Revision = Base.ExpectedRevision;
		std::atomic<uint32> Successes = 0;
		auto Publish = [&](const Durin::FPackagePath& InAssetPathValue) {
			FAssetRegistryPublication Publication = Base;
			FAssetData Data = MakeAssetData(InAssetPathValue);
			Publication.Assets.insert_or_assign(InAssetPathValue, std::move(Data));
			RebuildPackageProjection(Publication);
			if (PublishAssetRegistryPublication(std::move(Publication))) ++Successes;
		};
		std::thread First([&] { Publish(FirstPath); });
		std::thread Second([&] { Publish(SecondPath); });
		First.join();
		Second.join();
		EXPECT_EQ(Successes.load(), 1u);
		EXPECT_EQ(GetAssetCatalogRevision(), Revision + 1);
		FAssetRegistryPublication Stale = CaptureAssetRegistryPublication();
		Stale.ExpectedRevision = Revision;
		EXPECT_EQ(PublishAssetRegistryPublication(std::move(Stale)).Error,
			EAssetRegistryError::StaleData);

		const FAssetRegistrySnapshot Snapshot = CaptureAssetRegistrySnapshot();
		EXPECT_EQ(Snapshot.Revision, Snapshot.Catalog.Revision);
		EXPECT_EQ(Snapshot.Revision, Snapshot.References.GetRevision());
	}

	TEST(FAssetMetadataQueryTests, CapturesOnlyRevisionConsistentTransitiveDependencies)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests(
			"/MetadataTests/", "MetadataTests/");
		Durin::FPackagePath Root, Dependency, Unrelated;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/MetadataTests/ClosureRoot", Root));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/MetadataTests/ClosureDependency", Dependency));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/MetadataTests/ClosureUnrelated", Unrelated));

		FAssetRegistryPublication Publication = CaptureAssetRegistryPublication();
		Publication.Assets.insert_or_assign(Root,
			MakeAssetData(Root, {Dependency}));
		Publication.Assets.insert_or_assign(Dependency, MakeAssetData(Dependency));
		Publication.Assets.insert_or_assign(Unrelated, MakeAssetData(Unrelated));
		RebuildPackageProjection(Publication);
		ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));

		const FAssetDependencyClosureSnapshot Closure =
			CaptureAssetDependencyClosure(Root);
		ASSERT_TRUE(Closure) << Closure.Result.Message;
		EXPECT_EQ(Closure.Revision, GetAssetCatalogRevision());
		ASSERT_EQ(Closure.Assets.size(), 2u);
		EXPECT_EQ(std::ranges::count(Closure.Assets, Root,
			&FAssetData::PackagePath), 1);
		EXPECT_EQ(std::ranges::count(Closure.Assets, Dependency,
			&FAssetData::PackagePath), 1);
		EXPECT_EQ(std::ranges::count(Closure.Assets, Unrelated,
			&FAssetData::PackagePath), 0);
	}

	TEST(FAssetMetadataQueryTests, PublishesCanonicalPackageLevelDependenciesWithoutEngine)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests(
			"/MetadataTests/", "MetadataTests/");
		Durin::FPackagePath SourcePath, TargetPath, SoftPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/MetadataTests/ReferenceOwner", SourcePath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/MetadataTests/HardTarget", TargetPath));
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/MetadataTests/SoftTarget", SoftPath));

		Durin::FPackagePath RedirectPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(
			"/MetadataTests/Redirect", RedirectPath));
		FAssetRegistryPublication Publication = CaptureAssetRegistryPublication();
		Publication.Assets.insert_or_assign(SourcePath,
			MakeAssetData(SourcePath, {TargetPath}, {SoftPath}));
		FAssetData Redirect = MakeAssetData(RedirectPath, {TargetPath});
		Redirect.AssetClassName = "Durin::DAssetRedirector";
		Redirect.EntryKind = EAssetRegistryEntryKind::Redirector;
		Redirect.RedirectDestination = TargetPath;
		Publication.Assets.insert_or_assign(RedirectPath, std::move(Redirect));
		RebuildPackageProjection(Publication);
		ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));
		const FAssetReferenceIndex Index = CaptureAssetReferenceIndex();
		EXPECT_EQ(Index.FindTargets(SourcePath),
			(std::vector<Durin::FPackagePath>{TargetPath, SoftPath}));
		const auto Referencers = Index.FindReferencers(TargetPath);
		ASSERT_EQ(Referencers.size(), 2u);
		EXPECT_EQ(std::ranges::count(Referencers,
			EAssetReferenceKind::HardObject, &FAssetPackageReferenceEdge::Kind), 1);
		EXPECT_EQ(std::ranges::count(Referencers,
			EAssetReferenceKind::Redirect, &FAssetPackageReferenceEdge::Kind), 1);
	}
}
