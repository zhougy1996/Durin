#include "AssetRegistry/Catalog.h"
#include "DObject/PackageFormat.h"
#include "DObject/Package.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/ObjectGraphReplacement.h"
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
		std::vector<Durin::FPackagePath> Soft = {}) -> FPackageData
	{
		FTopLevelAssetPath AssetPath;
		EXPECT_TRUE(FTopLevelAssetPath::TryCreate(Path, "Asset", AssetPath));
		return {.PackagePath = Path,
			.TopLevelAssets = {{AssetPath, "Example::MetadataAsset", {}}},
			.FormatVersion = Durin::ObjectPackage::DastV10FormatVersion,
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
			VisitAssetPackageReferences(Data, Add);
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

	TEST(FAssetMetadataQueryTests, SnapshotOwnsExactMetadataWithoutEngine)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests(
			"/MetadataTests/", "MetadataTests/");
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/MetadataTests/Textures/Brick", Path));
		FTopLevelAssetPath TexturePath, MaterialPath;
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, "Texture", TexturePath));
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, "Material", MaterialPath));

		FAssetCatalogSnapshot Snapshot{
			.Revision = 17,
			.Assets = {{Path, FPackageData{
				.PackagePath = Path,
				.PhysicalPath = "Content/Textures/Brick.dasset",
				.TopLevelAssets = {{TexturePath, "Durin::DTexture2D", {}},
					{MaterialPath, "Durin::DMaterial", {}}},
				.FormatVersion = Durin::ObjectPackage::DastV10FormatVersion}}}};

		const FPackageData* Data = Snapshot.FindExact(Path);
		ASSERT_NE(Data, nullptr);
		EXPECT_EQ(Snapshot.Revision, 17u);
		EXPECT_EQ(Data->PackagePath, Path);
		EXPECT_EQ(Data->TopLevelAssets.front().AssetClassName, "Durin::DTexture2D");
		EXPECT_EQ(Data->FormatVersion, Durin::ObjectPackage::DastV10FormatVersion);
		const FAssetData* Texture = Snapshot.FindAssetExact(TexturePath);
		const FAssetData* Material = Snapshot.FindAssetExact(MaterialPath);
		ASSERT_NE(Texture, nullptr);
		ASSERT_NE(Material, nullptr);
		EXPECT_EQ(Texture->AssetClassName, "Durin::DTexture2D");
		EXPECT_EQ(Material->AssetClassName, "Durin::DMaterial");
	}

	TEST(FAssetMetadataQueryTests, OwnsCanonicalDastReaderIdentity)
	{
		EXPECT_EQ(Durin::ObjectPackage::DastV10FormatVersion, 10u);
		EXPECT_EQ(Durin::ObjectPackage::DastFormatName, "Durin.BinaryFormat.DAST");
		EXPECT_TRUE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(10));
		EXPECT_FALSE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(9));
		EXPECT_FALSE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(8));
		EXPECT_FALSE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(7));
		EXPECT_FALSE(Durin::ObjectPackage::IsSupportedPackageReaderVersion(6));

		const FAssetPackageFingerprint Fingerprint{
			.FileSize = 128,
			.LastWriteTimeTicks = 42,
			.ReaderVersion = Durin::ObjectPackage::DastV10FormatVersion};
		EXPECT_EQ(Fingerprint.ReaderVersion, 10u);
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
		First.Assets.insert_or_assign(Path, MakeAssetData(Path));
		RebuildPackageProjection(First);
		ASSERT_TRUE(PublishAssetRegistryPublication(std::move(First)));
		EXPECT_EQ(GetAssetCatalogRevision(), Revision + 1);
		ASSERT_TRUE(FindPackageExact(Path));

		FAssetRegistryPublication Stale = CaptureAssetRegistryPublication();
		Stale.ExpectedRevision = Revision;
		const auto Rejected = PublishAssetRegistryPublication(std::move(Stale));
		EXPECT_EQ(Rejected.Error, EAssetRegistryError::StaleData);
		EXPECT_EQ(Rejected.Context.Reason, EAssetRegistryFailure::PublicationRevision);
		EXPECT_EQ(Rejected.Context.Actual, Revision + 1);
		EXPECT_EQ(Rejected.Context.Expected, Revision);
		EXPECT_TRUE(FindPackageExact(Path));

		FAssetRegistryPublication Incomplete = CaptureAssetRegistryPublication();
		Incomplete.bReferenceIndexComplete = false;
		const auto IncompleteResult = PublishAssetRegistryPublication(std::move(Incomplete));
		EXPECT_EQ(IncompleteResult.Error, EAssetRegistryError::StaleData);
		EXPECT_EQ(IncompleteResult.Context.Reason, EAssetRegistryFailure::IncompleteProjection);
		EXPECT_EQ(GetAssetCatalogRevision(), Revision + 1);
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
			FPackageData Data = MakeAssetData(InAssetPathValue);
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
		const auto Rejected = PublishAssetRegistryPublication(std::move(Stale));
		EXPECT_EQ(Rejected.Error, EAssetRegistryError::StaleData);
		EXPECT_EQ(Rejected.Context.Reason, EAssetRegistryFailure::PublicationRevision);
		EXPECT_EQ(Rejected.Context.Actual, Revision + 1);
		EXPECT_EQ(Rejected.Context.Expected, Revision);

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
		ASSERT_TRUE(Closure) << FormatAssetRegistryError(Closure.Result);
		EXPECT_EQ(Closure.Revision, GetAssetCatalogRevision());
		ASSERT_EQ(Closure.Assets.size(), 2u);
		EXPECT_EQ(std::ranges::count(Closure.Assets, Root,
			&FPackageData::PackagePath), 1);
		EXPECT_EQ(std::ranges::count(Closure.Assets, Dependency,
			&FPackageData::PackagePath), 1);
		EXPECT_EQ(std::ranges::count(Closure.Assets, Unrelated,
			&FPackageData::PackagePath), 0);
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
		FPackageData Redirect = MakeAssetData(RedirectPath, {TargetPath});
		Redirect.TopLevelAssets.front().AssetClassName = "Durin::DAssetRedirector";
		ASSERT_TRUE(FSoftObjectPath::TryCreate(TargetPath.ToString() + ".Asset",
			Redirect.TopLevelAssets.front().RedirectDestination));
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

namespace
{
	TEST(FAssetMetadataQueryTests, UnsavedAssetQueriesOwnMetadataAndNotifyLifecycle)
	{
		Testing::InitializeDObjectSystemForTests();
		Testing::FScopedMountRegistryFixture Mounts;
		Testing::RegisterMountPointForTests("/QueryLive/", "QueryLive/");
		FPackagePath Path, Moved;
		ASSERT_TRUE(FPackagePath::TryCreate("/QueryLive/Original", Path));
		ASSERT_TRUE(FPackagePath::TryCreate("/QueryLive/Moved", Moved));
		const auto Start = GetAssetQueryRevision();
		TStrongObjectPtr<DPackage> Package(CreatePackage(Path));
		ASSERT_TRUE(Package);
		auto* Asset = NewObject<DObject>(Package.Get(), "Asset");
		FTopLevelAssetPath Original, Renamed, Relocated;
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, "Asset", Original));
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, "Renamed", Renamed));
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Moved, "Renamed", Relocated));
		const auto Snapshot = CaptureAssetQuerySnapshot();
		ASSERT_NE(Snapshot.FindAssetExact(Original), nullptr);
		EXPECT_EQ(Snapshot.FindPackageExact(Path), nullptr);
		const auto Entry = FindAssetExact(Original);
		ASSERT_TRUE(Entry);
		EXPECT_FALSE(Entry.Package);
		EXPECT_FALSE(FindAssetExact(Original, EAssetQuerySource::OnDisk));
		EXPECT_TRUE(FindAssetExact(Original, EAssetQuerySource::InMemory));
		auto Changes = CaptureAssetQueryChanges(Start);
		EXPECT_FALSE(Changes.bFullRefresh);
		EXPECT_TRUE(std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
			return Change.Kind == EContentChangeKind::Added && Change.NewAssetPath == Original.ToString();
		}));
		Asset->SetTransient(true);
		EXPECT_FALSE(FindAssetExact(Original));
		Asset->SetTransient(false);
		EXPECT_TRUE(FindAssetExact(Original));
		Package->SetPlayInEditor(true);
		EXPECT_FALSE(FindAssetExact(Original));
		Package->SetPlayInEditor(false);
		EXPECT_TRUE(FindAssetExact(Original));
		const auto BeforeRename = GetAssetQueryRevision();
		Asset->Rename("Renamed");
		EXPECT_FALSE(FindAssetExact(Original));
		EXPECT_TRUE(FindAssetExact(Renamed));
		Changes = CaptureAssetQueryChanges(BeforeRename);
		EXPECT_TRUE(std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
			return Change.Kind == EContentChangeKind::Renamed && Change.OldAssetPath == Original.ToString()
				&& Change.NewAssetPath == Renamed.ToString();
		}));
		ASSERT_TRUE(Package->RelocateAssetPackage(Moved));
		EXPECT_FALSE(FindAssetExact(Renamed));
		EXPECT_TRUE(FindAssetExact(Relocated));
		const auto BeforeRemoval = GetAssetQueryRevision();
		MarkObjectHierarchyAsGarbage(Package.Get());
		EXPECT_FALSE(FindAssetExact(Relocated));
		Changes = CaptureAssetQueryChanges(BeforeRemoval);
		EXPECT_TRUE(std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
			return Change.Kind == EContentChangeKind::Removed && Change.OldAssetPath == Relocated.ToString();
		}));
		EXPECT_NE(Snapshot.FindAssetExact(Original), nullptr);
	}

	TEST(FAssetMetadataQueryTests, LiveMetadataOverridesDiskWithoutInventingPackageMetadata)
	{
		Testing::InitializeDObjectSystemForTests();
		Testing::FScopedMountRegistryFixture Mounts;
		Testing::RegisterMountPointForTests("/QueryOverride/", "QueryOverride/");
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/QueryOverride/Asset", Path));
		auto Publication = CaptureAssetRegistryPublication();
		Publication.Assets.insert_or_assign(Path, MakeAssetData(Path));
		RebuildPackageProjection(Publication);
		ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));
		FTopLevelAssetPath AssetPath;
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, "Asset", AssetPath));
		const auto DiskClass = FindAssetExact(AssetPath, EAssetQuerySource::OnDisk)->AssetClassName;
		TStrongObjectPtr<DPackage> Package(CreatePackage(Path));
		EXPECT_FALSE(FindAssetExact(AssetPath)); // The empty live export set shadows disk.
		NewObject<DObject>(Package.Get(), "Asset");
		const auto All = CaptureAssetQuerySnapshot();
		ASSERT_NE(All.FindAssetExact(AssetPath), nullptr);
		EXPECT_NE(All.FindAssetExact(AssetPath)->AssetClassName, DiskClass);
		ASSERT_NE(All.FindPackageExact(Path), nullptr);
		EXPECT_EQ(All.FindPackageExact(Path)->TopLevelAssets.front().AssetClassName, DiskClass);
		const auto Revision = GetAssetQueryRevision();
		Package->MarkDirty();
		EXPECT_EQ(GetAssetQueryRevision(), Revision);
		Package->GetTopLevelAssets().front()->SetTransient(true);
		EXPECT_FALSE(FindAssetExact(AssetPath));
		MarkObjectHierarchyAsGarbage(Package.Get());
		ASSERT_TRUE(FindAssetExact(AssetPath));
		EXPECT_EQ(FindAssetExact(AssetPath)->AssetClassName, DiskClass);
		EXPECT_FALSE(FindAssetExact(AssetPath, EAssetQuerySource::InMemory));
	}

	TEST(FAssetMetadataQueryTests, PrivateCandidatesAppearOnlyAfterGraphApplication)
	{
		Testing::InitializeDObjectSystemForTests();
		Testing::FScopedMountRegistryFixture Mounts;
		Testing::RegisterMountPointForTests("/QueryPrivate/", "QueryPrivate/");
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/QueryPrivate/Asset", Path));
		TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "PrivateCandidate"));
		ASSERT_TRUE(Package->InitializePreparedAssetPackage(Path));
		NewObject<DObject>(Package.Get(), "Asset");
		FTopLevelAssetPath AssetPath;
		ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, "Asset", AssetPath));
		const auto Revision = GetAssetQueryRevision();
		EXPECT_FALSE(FindAssetExact(AssetPath));
		FObjectGraphReplacement Replacement;
		const FObjectReplacementPackagePair Pair{nullptr, Package.Get()};
		ASSERT_TRUE(Replacement.Prepare(std::span(&Pair, 1)));
		EXPECT_FALSE(FindAssetExact(AssetPath));
		ASSERT_TRUE(Replacement.TryCommit());
		EXPECT_TRUE(FindAssetExact(AssetPath));
		EXPECT_GT(GetAssetQueryRevision(), Revision);
		ASSERT_TRUE(Replacement.Retire());
		MarkObjectHierarchyAsGarbage(Package.Get());
	}
}
