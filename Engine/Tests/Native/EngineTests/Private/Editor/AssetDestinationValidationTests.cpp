#include "Misc/MountPathTestSupport.h"
#include "Import/AssetDestinationValidation.h"

#include "EngineTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor;

	auto EmptyOccupancy(const FPackagePath&) -> FAssetDestinationOccupancy
	{
		return {};
	}

	auto RegistryOccupancy(const FPackagePath&) -> FAssetDestinationOccupancy
	{
		return {.bRegistryAssetExists = true};
	}

	auto PublishedPackageOccupancy(const FPackagePath&) -> FAssetDestinationOccupancy
	{
		return {.bResidentPackageExists = true};
	}

	auto NewlyCreatedPackageOccupancy(const FPackagePath&)
		-> FAssetDestinationOccupancy
	{
		return {
			.bResidentPackageExists = true,
			.bResidentPackageNewlyCreated = true};
	}

	auto RedirectorOccupancy(const FPackagePath&) -> FAssetDestinationOccupancy
	{
		FPackagePath Destination;
		(void)FPackagePath::TryCreate("/Project/Textures/Final", Destination);
		return {
			.bRegistryAssetExists = true,
			.OccupantKind = EAssetDestinationOccupantKind::Redirector,
			.RedirectDestination = Destination};
	}

	class FAssetDestinationValidationTests : public testing::Test
	{
	protected:
		void SetUp() override
		{
			Root = Durin::Testing::GetTestWorkDirectory() / "AssetDestinationValidation";
			std::filesystem::create_directories(Root / "Project/Content");
			std::filesystem::create_directories(Root / "Engine/Content");
			std::filesystem::create_directories(Root / "Sources");
			std::filesystem::create_directories(Root / "CanonicalContent");
			const std::array Definitions{
				FMountPoint{
					.VirtualRoot = "/Project/",
					.Owner = EMountOwner::ActiveProject,
					.Root = Root / "Project/Content",
					.bAutoScan = true,
					.bContentWritable = true},
				FMountPoint{
					.VirtualRoot = "/Engine/",
					.Owner = EMountOwner::Engine,
					.Root = Root / "Engine/Content",
					.bAutoScan = true},
				FMountPoint{
					.VirtualRoot = "/Sources/",
					.Owner = EMountOwner::ExternalSources,
					.Root = Root / "Sources"},
				FMountPoint{
					.VirtualRoot = "/NonNormalized/",
					.Owner = EMountOwner::Extension,
					.Root = Root / "Project/../CanonicalContent",
					.bAutoScan = true,
					.bContentWritable = true}};
			Registry = std::make_unique<Testing::FScopedMountRegistryFixture>(Definitions);
			ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();
		}

		std::filesystem::path Root;
		std::unique_ptr<Testing::FScopedMountRegistryFixture> Registry;
	};
} // namespace

TEST_F(FAssetDestinationValidationTests, ResolvesCanonicalAndPhysicalDestination)
{
	const FAssetDestinationValidation Result =
		InspectAssetDestination("/project/Textures/Stone", EmptyOccupancy);
	ASSERT_TRUE(Result) << FormatAssetDestinationValidation(Result);
	EXPECT_EQ(Result.AssetPath.ToString(), "/project/Textures/Stone");
	ASSERT_NE(Result.Mount, nullptr);
	EXPECT_EQ(Result.Mount->VirtualRoot, "/Project/");
	EXPECT_EQ(Result.PhysicalPath.lexically_normal(),
		(Root / "Project/Content/Textures/Stone.dasset").lexically_normal());

	const FAssetDestinationValidation NonNormalizedRoot =
		InspectAssetDestination("/NonNormalized/Textures/Stone", EmptyOccupancy);
	ASSERT_TRUE(NonNormalizedRoot) << FormatAssetDestinationValidation(NonNormalizedRoot);
	EXPECT_EQ(NonNormalizedRoot.PhysicalPath.lexically_normal(),
		(Root / "CanonicalContent/Textures/Stone.dasset").lexically_normal());
}

TEST_F(FAssetDestinationValidationTests, RejectsInvalidUnknownAndLookalikePaths)
{
	const FAssetDestinationValidation Trailing =
		InspectAssetDestination("/Project/Textures/", EmptyOccupancy);
	EXPECT_FALSE(Trailing.bAssetPathValid);
	EXPECT_EQ(Trailing.Error, EAssetDestinationError::Path);

	const FAssetDestinationValidation Unknown =
		InspectAssetDestination("/Unknown/Textures/Stone", EmptyOccupancy);
	EXPECT_FALSE(Unknown.bAssetPathValid);
	EXPECT_FALSE(Unknown.bMountedDestination);
	EXPECT_EQ(Unknown.Error, EAssetDestinationError::Path);
	ASSERT_TRUE(Unknown.PathCause);
	EXPECT_EQ(Unknown.RequestedPath, "/Unknown/Textures/Stone");

	const FAssetDestinationValidation Lookalike =
		InspectAssetDestination("/ProjectExtra/Textures/Stone", EmptyOccupancy);
	EXPECT_FALSE(Lookalike.bAssetPathValid);
	EXPECT_FALSE(Lookalike.bMountedDestination);
	EXPECT_EQ(Lookalike.Error, EAssetDestinationError::Path);

	const FAssetDestinationValidation ReadOnlySource =
		InspectAssetDestination("/Sources/Textures/Stone", EmptyOccupancy);
	EXPECT_TRUE(ReadOnlySource.bAssetPathValid);
	EXPECT_TRUE(ReadOnlySource.bMountedDestination);
	EXPECT_FALSE(ReadOnlySource.bContentWritable);
	EXPECT_FALSE(ReadOnlySource);
	EXPECT_EQ(ReadOnlySource.Error, EAssetDestinationError::ReadOnly);

	const FAssetDestinationValidation ReadOnlyEngine =
		InspectAssetDestination("/Engine/Textures/Stone", EmptyOccupancy);
	EXPECT_TRUE(ReadOnlyEngine.bMountedDestination);
	EXPECT_FALSE(ReadOnlyEngine.bContentWritable);
	EXPECT_FALSE(ReadOnlyEngine);
}

TEST_F(FAssetDestinationValidationTests, ReportsRegistryAndLoadedPackageCollisions)
{
	const FAssetDestinationValidation RegistryResult =
		InspectAssetDestination("/Project/Textures/Registered", RegistryOccupancy);
	EXPECT_TRUE(RegistryResult.bRegistryAssetExists);
	EXPECT_FALSE(RegistryResult.bResidentPackageExists);
	EXPECT_FALSE(RegistryResult);
	EXPECT_EQ(RegistryResult.Error, EAssetDestinationError::RegistryAsset);

	const FAssetDestinationValidation LoadedResult =
		InspectAssetDestination("/Project/Textures/Loaded", PublishedPackageOccupancy);
	EXPECT_FALSE(LoadedResult.bRegistryAssetExists);
	EXPECT_TRUE(LoadedResult.bResidentPackageExists);
	EXPECT_FALSE(LoadedResult.bResidentPackageNewlyCreated);
	EXPECT_FALSE(LoadedResult);
	EXPECT_EQ(LoadedResult.Error, EAssetDestinationError::ResidentPackage);

	const FAssetDestinationValidation DraftResult =
		InspectAssetDestination(
			"/Project/Textures/Draft", NewlyCreatedPackageOccupancy);
	EXPECT_FALSE(DraftResult.bRegistryAssetExists);
	EXPECT_TRUE(DraftResult.bResidentPackageExists);
	EXPECT_TRUE(DraftResult.bResidentPackageNewlyCreated);
	EXPECT_FALSE(DraftResult);
	EXPECT_EQ(DraftResult.Error, EAssetDestinationError::UnsavedPackage);

	const FAssetDestinationValidation RedirectorResult =
		InspectAssetDestination(
			"/Project/Textures/Redirected", RedirectorOccupancy);
	EXPECT_FALSE(RedirectorResult);
	EXPECT_EQ(RedirectorResult.Error, EAssetDestinationError::Redirector);
	EXPECT_EQ(RedirectorResult.RedirectDestination.ToString(), "/Project/Textures/Final");
	ASSERT_TRUE(InspectAssetDestination(RedirectorResult.RequestedPath, EmptyOccupancy));
	EXPECT_EQ(RedirectorResult.Error, EAssetDestinationError::Redirector);
	EXPECT_EQ(
		RedirectorResult.OccupantKind,
		EAssetDestinationOccupantKind::Redirector);


}

TEST_F(FAssetDestinationValidationTests, ClassifiesNormalizedAndNonNormalizedPhysicalPaths)
{
	const FAssetDestinationValidation Normalized = ClassifyAssetDestination(
		Root / "Project/Content/Textures/Stone.dasset", EmptyOccupancy);
	ASSERT_TRUE(Normalized) << FormatAssetDestinationValidation(Normalized);
	EXPECT_EQ(Normalized.AssetPath.ToString(), "/Project/Textures/Stone");

	const FAssetDestinationValidation NonNormalized = ClassifyAssetDestination(
		Root / "Project/Content/Textures/../Materials/Stone.dasset", EmptyOccupancy);
	ASSERT_TRUE(NonNormalized) << FormatAssetDestinationValidation(NonNormalized);
	EXPECT_EQ(NonNormalized.AssetPath.ToString(), "/Project/Materials/Stone");

	const FAssetDestinationValidation Outside = ClassifyAssetDestination(
		Root / "Project/ContentLookalike/Stone.dasset", EmptyOccupancy);
	EXPECT_FALSE(Outside.bMountedDestination);
	EXPECT_FALSE(FormatAssetDestinationValidation(Outside).empty());
}

TEST_F(FAssetDestinationValidationTests, ResolvesVirtualContentDirectories)
{
	const FContentDirectoryValidation Virtual =
		InspectContentDirectory("/Project/Scenes/Robot");
	ASSERT_TRUE(Virtual) << FormatContentDirectoryValidation(Virtual);
	EXPECT_EQ(Virtual.DirectoryPath.ToString(), "/Project/Scenes/Robot");
	EXPECT_EQ(Virtual.PhysicalPath.lexically_normal(),
		(Root / "Project/Content/Scenes/Robot").lexically_normal());

	const FContentDirectoryValidation Physical = ClassifyContentDirectory(
		Root / "Project/Content/Scenes/Robot");
	ASSERT_TRUE(Physical) << FormatContentDirectoryValidation(Physical);
	EXPECT_EQ(Physical.DirectoryPath.ToString(), "/Project/Scenes/Robot");

	const FContentDirectoryValidation Outside = ClassifyContentDirectory(
		Root / "Project/ContentLookalike/Scenes/Robot");
	EXPECT_FALSE(Outside);
	EXPECT_EQ(Outside.Error, EContentDirectoryError::Mount);
	EXPECT_NE(Outside.MountCause, EMountPathError::None);

	const FContentDirectoryValidation ReadOnly =
		InspectContentDirectory("/Sources/Scenes/Robot");
	EXPECT_TRUE(ReadOnly.bMountedDestination);
	EXPECT_FALSE(ReadOnly.bContentWritable);
	EXPECT_FALSE(ReadOnly);
	EXPECT_EQ(ReadOnly.Error, EContentDirectoryError::ReadOnly);
	EXPECT_EQ(ReadOnly.RequestedPath, "/Sources/Scenes/Robot");
}
