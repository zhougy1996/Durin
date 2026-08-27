#include "Editor/Import/AssetDestinationValidation.h"

#include "EngineTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor;

	auto EmptyOccupancy(const FAssetPath&) -> FAssetDestinationOccupancy
	{
		return {};
	}

	auto RegistryOccupancy(const FAssetPath&) -> FAssetDestinationOccupancy
	{
		return {.bRegistryAssetExists = true};
	}

	auto PublishedPackageOccupancy(const FAssetPath&) -> FAssetDestinationOccupancy
	{
		return {.ResidentPublicationState =
			Asset::EAssetPackagePublicationState::Published};
	}

	auto NewlyCreatedPackageOccupancy(const FAssetPath&)
		-> FAssetDestinationOccupancy
	{
		return {.ResidentPublicationState =
			Asset::EAssetPackagePublicationState::NewlyCreated};
	}

	auto RedirectorOccupancy(const FAssetPath&) -> FAssetDestinationOccupancy
	{
		FAssetPath Destination;
		(void)FAssetPath::TryCreate("/Project/Textures/Final", Destination);
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
				PathUtilities::FMountPoint{
					.VirtualRoot = "/Project/",
					.Owner = PathUtilities::EMountOwner::ActiveProject,
					.Root = Root / "Project/Content",
					.bAutoScan = true,
					.bContentWritable = true},
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
					.bAutoScan = true,
					.bContentWritable = true}};
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

	const FAssetDestinationValidation ReadOnlySource =
		InspectAssetDestination("/Sources/Textures/Stone", EmptyOccupancy);
	EXPECT_TRUE(ReadOnlySource.bAssetPathValid);
	EXPECT_TRUE(ReadOnlySource.bMountedDestination);
	EXPECT_FALSE(ReadOnlySource.bContentWritable);
	EXPECT_FALSE(ReadOnlySource);
	EXPECT_EQ(ReadOnlySource.Message,
		"Choose a destination inside a content-writable mount.");

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
	EXPECT_FALSE(RegistryResult.ResidentPublicationState.has_value());
	EXPECT_FALSE(RegistryResult);
	EXPECT_EQ(
		RegistryResult.Message,
		"An asset already exists at this path. Choose another destination or delete the existing asset first.");

	const FAssetDestinationValidation LoadedResult =
		InspectAssetDestination("/Project/Textures/Loaded", PublishedPackageOccupancy);
	EXPECT_FALSE(LoadedResult.bRegistryAssetExists);
	EXPECT_EQ(
		LoadedResult.ResidentPublicationState,
		Asset::EAssetPackagePublicationState::Published);
	EXPECT_FALSE(LoadedResult);
	EXPECT_EQ(
		LoadedResult.Message,
		"A resident package already uses this path. Close it or choose another destination.");

	const FAssetDestinationValidation DraftResult =
		InspectAssetDestination(
			"/Project/Textures/Draft", NewlyCreatedPackageOccupancy);
	EXPECT_FALSE(DraftResult.bRegistryAssetExists);
	EXPECT_EQ(
		DraftResult.ResidentPublicationState,
		Asset::EAssetPackagePublicationState::NewlyCreated);
	EXPECT_FALSE(DraftResult);
	EXPECT_EQ(
		DraftResult.Message,
		"A newly created unsaved package already uses this path. Save or explicitly discard it before reusing the destination.");

	const FAssetDestinationValidation RedirectorResult =
		InspectAssetDestination(
			"/Project/Textures/Redirected", RedirectorOccupancy);
	EXPECT_FALSE(RedirectorResult);
	EXPECT_EQ(
		RedirectorResult.OccupantKind,
		EAssetDestinationOccupantKind::Redirector);
	EXPECT_NE(RedirectorResult.Message.find("/Project/Textures/Final"),
		std::string::npos);
	EXPECT_NE(RedirectorResult.Message.find("Fix Up Redirectors"),
		std::string::npos);
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

	const FContentDirectoryValidation ReadOnly =
		InspectContentDirectory("/Sources/Scenes/Robot");
	EXPECT_TRUE(ReadOnly.bMountedDestination);
	EXPECT_FALSE(ReadOnly.bContentWritable);
	EXPECT_FALSE(ReadOnly);
	EXPECT_EQ(ReadOnly.Message,
		"Choose a directory inside a content-writable mount.");
}
