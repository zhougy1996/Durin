#include "Assets/AssetDestinationValidation.h"

#include "EngineTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	auto EmptyOccupancy(const FAssetPath&) -> FAssetDestinationOccupancy
	{
		return {};
	}

	auto RegistryOccupancy(const FAssetPath&) -> FAssetDestinationOccupancy
	{
		return {.bRegistryAssetExists = true};
	}

	auto LoadedPackageOccupancy(const FAssetPath&) -> FAssetDestinationOccupancy
	{
		return {.bLoadedPackageExists = true};
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
				PathUtilities::FMountPoint{
					.VirtualRoot = "/Project/",
					.Owner = PathUtilities::EMountOwner::ActiveProject,
					.Root = Root / "Project/Content",
					.bAutoScan = true},
				PathUtilities::FMountPoint{
					.VirtualRoot = "/Engine/",
					.Owner = PathUtilities::EMountOwner::Engine,
					.Root = Root / "Engine/Content",
					.bAutoScan = true},
				PathUtilities::FMountPoint{
					.VirtualRoot = "/Sources/",
					.Owner = PathUtilities::EMountOwner::ExternalSources,
					.Root = Root / "Sources"},
				PathUtilities::FMountPoint{
					.VirtualRoot = "/NonNormalized/",
					.Owner = PathUtilities::EMountOwner::Extension,
					.Root = Root / "Project/../CanonicalContent",
					.bAutoScan = true}};
			Registry = std::make_unique<PathUtilities::FScopedMountRegistryFixture>(Definitions);
			ASSERT_TRUE(Registry->IsValid()) << Registry->GetError();
		}

		std::filesystem::path Root;
		std::unique_ptr<PathUtilities::FScopedMountRegistryFixture> Registry;
	};
} // namespace

TEST_F(FAssetDestinationValidationTests, ResolvesCanonicalAndPhysicalDestination)
{
	const FAssetDestinationValidation Result =
		InspectAssetDestination("/project/Textures/Stone", EmptyOccupancy);
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.AssetPath.ToString(), "/project/Textures/Stone");
	ASSERT_NE(Result.Mount, nullptr);
	EXPECT_EQ(Result.Mount->VirtualRoot, "/Project/");
	EXPECT_EQ(Result.PhysicalPath.lexically_normal(),
		(Root / "Project/Content/Textures/Stone.dasset").lexically_normal());

	const FAssetDestinationValidation NonNormalizedRoot =
		InspectAssetDestination("/NonNormalized/Textures/Stone", EmptyOccupancy);
	ASSERT_TRUE(NonNormalizedRoot) << NonNormalizedRoot.Message;
	EXPECT_EQ(NonNormalizedRoot.PhysicalPath.lexically_normal(),
		(Root / "CanonicalContent/Textures/Stone.dasset").lexically_normal());
}

TEST_F(FAssetDestinationValidationTests, RejectsInvalidUnknownAndLookalikePaths)
{
	const FAssetDestinationValidation Trailing =
		InspectAssetDestination("/Project/Textures/", EmptyOccupancy);
	EXPECT_FALSE(Trailing.bAssetPathValid);
	EXPECT_FALSE(Trailing.Message.empty());

	const FAssetDestinationValidation Unknown =
		InspectAssetDestination("/Unknown/Textures/Stone", EmptyOccupancy);
	EXPECT_FALSE(Unknown.bAssetPathValid);
	EXPECT_FALSE(Unknown.bMountedDestination);
	EXPECT_EQ(Unknown.Message, "Virtual path does not use a registered mount.");

	const FAssetDestinationValidation Lookalike =
		InspectAssetDestination("/ProjectExtra/Textures/Stone", EmptyOccupancy);
	EXPECT_FALSE(Lookalike.bAssetPathValid);
	EXPECT_FALSE(Lookalike.bMountedDestination);
	EXPECT_EQ(Lookalike.Message, "Virtual path does not use a registered mount.");

	const FAssetDestinationValidation ManualScan =
		InspectAssetDestination("/Sources/Textures/Stone", EmptyOccupancy);
	EXPECT_TRUE(ManualScan.bAssetPathValid);
	EXPECT_TRUE(ManualScan.bMountedDestination);
	EXPECT_TRUE(ManualScan);
}

TEST_F(FAssetDestinationValidationTests, ReportsRegistryAndLoadedPackageCollisions)
{
	const FAssetDestinationValidation RegistryResult =
		InspectAssetDestination("/Project/Textures/Registered", RegistryOccupancy);
	EXPECT_TRUE(RegistryResult.bRegistryAssetExists);
	EXPECT_FALSE(RegistryResult.bLoadedPackageExists);
	EXPECT_FALSE(RegistryResult);
	EXPECT_EQ(RegistryResult.Message, "An asset already exists at this path.");

	const FAssetDestinationValidation LoadedResult =
		InspectAssetDestination("/Project/Textures/Loaded", LoadedPackageOccupancy);
	EXPECT_FALSE(LoadedResult.bRegistryAssetExists);
	EXPECT_TRUE(LoadedResult.bLoadedPackageExists);
	EXPECT_FALSE(LoadedResult);
	EXPECT_EQ(LoadedResult.Message, "An asset already exists at this path.");
}

TEST_F(FAssetDestinationValidationTests, ClassifiesNormalizedAndNonNormalizedPhysicalPaths)
{
	const FAssetDestinationValidation Normalized = ClassifyAssetDestination(
		Root / "Project/Content/Textures/Stone.dasset", EmptyOccupancy);
	ASSERT_TRUE(Normalized) << Normalized.Message;
	EXPECT_EQ(Normalized.AssetPath.ToString(), "/Project/Textures/Stone");

	const FAssetDestinationValidation NonNormalized = ClassifyAssetDestination(
		Root / "Project/Content/Textures/../Materials/Stone.dasset", EmptyOccupancy);
	ASSERT_TRUE(NonNormalized) << NonNormalized.Message;
	EXPECT_EQ(NonNormalized.AssetPath.ToString(), "/Project/Materials/Stone");

	const FAssetDestinationValidation Outside = ClassifyAssetDestination(
		Root / "Project/ContentLookalike/Stone.dasset", EmptyOccupancy);
	EXPECT_FALSE(Outside.bMountedDestination);
	EXPECT_FALSE(Outside.Message.empty());
}

TEST_F(FAssetDestinationValidationTests, ResolvesVirtualContentDirectories)
{
	const FContentDirectoryValidation Virtual =
		InspectContentDirectory("/Project/Scenes/Robot");
	ASSERT_TRUE(Virtual) << Virtual.Message;
	EXPECT_EQ(Virtual.DirectoryPath.ToString(), "/Project/Scenes/Robot");
	EXPECT_EQ(Virtual.PhysicalPath.lexically_normal(),
		(Root / "Project/Content/Scenes/Robot").lexically_normal());

	const FContentDirectoryValidation Physical = ClassifyContentDirectory(
		Root / "Project/Content/Scenes/Robot");
	ASSERT_TRUE(Physical) << Physical.Message;
	EXPECT_EQ(Physical.DirectoryPath.ToString(), "/Project/Scenes/Robot");

	const FContentDirectoryValidation Outside = ClassifyContentDirectory(
		Root / "Project/ContentLookalike/Scenes/Robot");
	EXPECT_FALSE(Outside);
	EXPECT_FALSE(Outside.Message.empty());
}
