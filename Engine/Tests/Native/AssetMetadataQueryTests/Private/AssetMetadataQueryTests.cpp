#include "AssetRegistry/Catalog.h"
#include "AssetRegistry/PackageFormat.h"
#include "AssetRegistry/PackageHeader.h"
#include "AssetRegistry/PackageTypes.h"
#include "AssetRegistry/ObjectStream.h"
#include "AssetRegistry/Publication.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin::Asset;

	TEST(FAssetMetadataQueryTests, SnapshotOwnsExactMetadataWithoutEngine)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests(
			"/MetadataTests/", "MetadataTests/");
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MetadataTests/Textures/Brick", Path));

		FAssetCatalogSnapshot Snapshot{
			.Revision = 17,
			.Assets = {{Path, FAssetData{
				.PackagePath = Path,
				.PhysicalPath = "Content/Textures/Brick.dasset",
				.AssetClassName = "Durin::DTexture2D",
				.FormatVersion = 7}}}};

		const FAssetData* Data = Snapshot.FindExact(Path);
		ASSERT_NE(Data, nullptr);
		EXPECT_EQ(Snapshot.Revision, 17u);
		EXPECT_EQ(Data->PackagePath, Path);
		EXPECT_EQ(Data->AssetClassName, "Durin::DTexture2D");
		EXPECT_EQ(Data->FormatVersion, 7u);
	}

	TEST(FAssetMetadataQueryTests, OwnsCanonicalDastReaderIdentity)
	{
		EXPECT_EQ(AssetPackageV7FormatVersion, 7u);
		EXPECT_EQ(DastBinaryFormatName, "Durin.BinaryFormat.DAST");
		EXPECT_TRUE(IsSupportedAssetPackageReaderVersion(7));
		EXPECT_FALSE(IsSupportedAssetPackageReaderVersion(6));

		const FAssetPackageFingerprint Fingerprint{
			.FileSize = 128,
			.LastWriteTimeTicks = 42,
			.ReaderVersion = AssetPackageV7FormatVersion};
		EXPECT_EQ(Fingerprint.ReaderVersion, 7u);
	}

	TEST(FAssetMetadataQueryTests, RejectsMalformedPublicSummaryWithoutEngine)
	{
		Dast::FPublicSummary Summary;
		std::string Error;
		EXPECT_FALSE(Dast::DecodePublicSummary({}, {},
			EAssetRegistryEntryKind::Asset, Summary, &Error));
		EXPECT_EQ(Error, "DAST Public Summary is malformed.");
	}

	TEST(FAssetMetadataQueryTests, PublishesWholeStateAgainstExpectedRevision)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests("/MetadataTests/", "MetadataTests/");
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MetadataTests/Published", Path));

		FAssetRegistryPublication First = CaptureAssetRegistryPublication();
		const uint64 Revision = First.ExpectedRevision;
		First.Assets.insert_or_assign(Path, FAssetData{
			.PackagePath = Path,
			.AssetClassName = "Durin::DTexture2D",
			.FormatVersion = AssetPackageV7FormatVersion});
		First.ReferenceFingerprints.insert_or_assign(Path, FAssetPackageFingerprint{});
		First.ReferenceErrors.clear();
		First.bReferenceIndexComplete = true;
		ASSERT_TRUE(PublishAssetRegistryPublication(std::move(First)));
		EXPECT_EQ(GetAssetCatalogRevision(), Revision + 1);
		ASSERT_TRUE(FindAssetExact(Path));

		FAssetRegistryPublication Stale = CaptureAssetRegistryPublication();
		Stale.ExpectedRevision = Revision;
		EXPECT_EQ(PublishAssetRegistryPublication(std::move(Stale)).Error,
			EAssetError::StaleData);
		EXPECT_TRUE(FindAssetExact(Path));

		FAssetRegistryPublication Incomplete = CaptureAssetRegistryPublication();
		Incomplete.bReferenceIndexComplete = false;
		EXPECT_EQ(PublishAssetRegistryPublication(std::move(Incomplete)).Error,
			EAssetError::StaleData);
	}

	TEST(FAssetMetadataQueryTests, ConcurrentExpectedRevisionPublishesAtMostOnce)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests("/MetadataTests/", "MetadataTests/");
		Durin::FAssetPath FirstPath, SecondPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MetadataTests/ConcurrentA", FirstPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/MetadataTests/ConcurrentB", SecondPath));
		const FAssetRegistryPublication Base = CaptureAssetRegistryPublication();
		const uint64 Revision = Base.ExpectedRevision;
		std::atomic<uint32> Successes = 0;
		auto Publish = [&](const Durin::FAssetPath& InAssetPathValue) {
			FAssetRegistryPublication Publication = Base;
			FAssetData Data;
			Data.PackagePath = InAssetPathValue;
			Publication.Assets.insert_or_assign(InAssetPathValue, std::move(Data));
			Publication.ReferenceFingerprints.insert_or_assign(
				InAssetPathValue, FAssetPackageFingerprint{});
			Publication.bReferenceIndexComplete = true;
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
			EAssetError::StaleData);

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
		Durin::FAssetPath Root, Dependency, Unrelated;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MetadataTests/ClosureRoot", Root));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MetadataTests/ClosureDependency", Dependency));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MetadataTests/ClosureUnrelated", Unrelated));

		FAssetRegistryPublication Publication = CaptureAssetRegistryPublication();
		Publication.Assets.insert_or_assign(Root, FAssetData{
			.PackagePath = Root,
			.Dependencies = {Dependency}});
		Publication.Assets.insert_or_assign(Dependency, FAssetData{
			.PackagePath = Dependency});
		Publication.Assets.insert_or_assign(Unrelated, FAssetData{
			.PackagePath = Unrelated});
		for (const Durin::FAssetPath& Path : {Root, Dependency, Unrelated})
			Publication.ReferenceFingerprints.insert_or_assign(
				Path, FAssetPackageFingerprint{});
		Publication.ReferenceErrors.clear();
		Publication.bReferenceIndexComplete = true;
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

	TEST(FAssetMetadataQueryTests, ExtractsCanonicalObjectStreamReferencesWithoutEngine)
	{
		Durin::Testing::InitializeDObjectSystemForTests();
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::Testing::RegisterMountPointForTests(
			"/MetadataTests/", "MetadataTests/");
		Durin::FAssetPath SourcePath, TargetPath, SoftPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MetadataTests/ReferenceOwner", SourcePath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MetadataTests/HardTarget", TargetPath));
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MetadataTests/SoftTarget", SoftPath));

		namespace ObjectStream = Durin::Asset::PackageObjectStream;
		auto Hard = ObjectStream::MakeType(
			ObjectStream::ETypeOpcode::HardRef, "DObject");
		auto Soft = ObjectStream::MakeType(
			ObjectStream::ETypeOpcode::SoftRef, "DObject");
		ObjectStream::FPackageInput Input{
			.AssetClass = "Example::MetadataAsset",
			.Dependencies = {TargetPath.ToString()},
			.AdditionalNames = {SoftPath.ToString()},
			.Types = {Hard, Soft},
			.Schemas = {{"Example::MetadataAsset",
				{{"Hard", Hard, 0}, {"Soft", Soft, 0}}}},
			.Objects = {{"Root", {}, "Example::MetadataAsset", "Root"}},
			.ObjectValues = {{"Root", {
				{.SchemaName = "Example::MetadataAsset", .FieldName = "Hard",
					.Value = ObjectStream::FValue{
						.ReferenceTag = 2, .ReferenceId = 1}},
				{.SchemaName = "Example::MetadataAsset", .FieldName = "Soft",
					.Value = ObjectStream::FValue{
						.Text = SoftPath.ToString(), .ReferenceTag = 1}},
			}}},
		};
		std::vector<std::byte> Bytes;
		ObjectStream::FWriterDiagnostic WriterDiagnostic;
		ASSERT_TRUE(ObjectStream::WritePackage(Input, Bytes, &WriterDiagnostic))
			<< WriterDiagnostic.Message;
		std::vector<FAssetReferenceEdge> References;
		ObjectStream::FReaderDiagnostic ReaderDiagnostic;
		ASSERT_TRUE(ObjectStream::ExtractReferences(
			Bytes, SourcePath, References, {}, &ReaderDiagnostic))
			<< ReaderDiagnostic.Message;
		ASSERT_EQ(References.size(), 2u);
		EXPECT_EQ(References[0].SourcePackage, SourcePath);
		EXPECT_EQ(std::ranges::count(
			References, TargetPath, &FAssetReferenceEdge::TargetPath), 1);
		EXPECT_EQ(std::ranges::count(
			References, SoftPath, &FAssetReferenceEdge::TargetPath), 1);

		Durin::FAssetPath RedirectPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(
			"/MetadataTests/Redirect", RedirectPath));
		References.push_back({
			.SourcePackage = RedirectPath,
			.Kind = EAssetReferenceKind::Redirect,
			.TargetPath = TargetPath,
			.DisplayRoute = "RedirectDestination"});
		FAssetRegistryPublication Publication = CaptureAssetRegistryPublication();
		for (const Durin::FAssetPath& Source : std::array{SourcePath, RedirectPath})
		{
			Publication.Assets.insert_or_assign(Source, FAssetData{
				.PackagePath = Source,
				.AssetClassName = "Example::MetadataAsset",
				.FormatVersion = AssetPackageV7FormatVersion});
			Publication.ReferenceFingerprints.insert_or_assign(
				Source, FAssetPackageFingerprint{});
		}
		for (FAssetReferenceEdge& Reference : References)
			Reference.SourceFingerprint = FAssetPackageFingerprint{};
		Publication.ReferenceEdges.insert(Publication.ReferenceEdges.end(),
			References.begin(), References.end());
		Publication.ReferenceErrors.clear();
		Publication.bReferenceIndexComplete = true;
		ASSERT_TRUE(PublishAssetRegistryPublication(std::move(Publication)));
		const FAssetReferenceIndex Index = CaptureAssetReferenceIndex();
		EXPECT_EQ(Index.FindTargets(SourcePath),
			(std::vector<Durin::FAssetPath>{TargetPath, SoftPath}));
		const auto Referencers = Index.FindReferencers(TargetPath);
		ASSERT_EQ(Referencers.size(), 2u);
		EXPECT_EQ(std::ranges::count(Referencers,
			EAssetReferenceKind::HardObject, &FAssetReferenceEdge::Kind), 1);
		EXPECT_EQ(std::ranges::count(Referencers,
			EAssetReferenceKind::Redirect, &FAssetReferenceEdge::Kind), 1);
	}
}
