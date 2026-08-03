#include "Assets/ImportDialogState.h"

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
} // namespace

TEST(FImportDialogCallbacksTests, RoutesWorkspaceOutcomes)
{
	bool bCleared = false;
	std::string Error;
	std::string ImportedPath;
	std::string ImportedDirectory;
	const FImportDialogCallbacks Callbacks{
		.ClearError = [&bCleared] { bCleared = true; },
		.ReportError = [&Error](std::string Message) {
			Error = std::move(Message);
		},
		.Imported = [&ImportedPath](std::string AssetPath) {
			ImportedPath = std::move(AssetPath);
		},
		.ImportedDirectory = [&ImportedDirectory](std::string DirectoryPath) {
			ImportedDirectory = std::move(DirectoryPath);
		},
	};

	Callbacks.Clear();
	Callbacks.Report("Import failed.");
	Callbacks.NotifyImported("/Project/Textures/Stone");
	Callbacks.NotifyImportedDirectory("/Project/Scenes/Robot");

	EXPECT_TRUE(bCleared);
	EXPECT_EQ(Error, "Import failed.");
	EXPECT_EQ(ImportedPath, "/Project/Textures/Stone");
	EXPECT_EQ(ImportedDirectory, "/Project/Scenes/Robot");
}

TEST(FImportDialogDestinationModelTests, PreservesManualPathAcrossSuggestions)
{
	FImportDialogDestinationModel Destination;
	Destination.Reset("/Project/Chosen");
	Destination.SuggestPath(
		Destination.MakeSuggestedPath("First", "/Project/Textures/"));
	EXPECT_EQ(Destination.GetPath(), "/Project/Chosen/First");

	Destination.SuggestPath(
		Destination.MakeSuggestedPath("Second", "/Project/Textures/"));
	EXPECT_EQ(Destination.GetPath(), "/Project/Chosen/Second");

	ASSERT_TRUE(Destination.SetPath("/Project/Manual"));
	Destination.SuggestPath(
		Destination.MakeSuggestedPath("Third", "/Project/Textures/"));
	EXPECT_EQ(Destination.GetPath(), "/Project/Manual");

	const std::string TooLong(
		FImportDialogDestinationModel::AssetPathCapacity, 'x');
	EXPECT_FALSE(Destination.SetPath(TooLong));
	EXPECT_EQ(Destination.GetPath(), "/Project/Manual");
}

TEST(FImportDialogDirectoryModelTests, SuggestsSceneDirectoryAndPreservesManualPath)
{
	FImportDialogDirectoryModel Directory;
	Directory.Reset("/Project/Chosen");
	Directory.SuggestPath(
		Directory.MakeSuggestedPath("Robot", "/Project/Scenes/"));
	EXPECT_EQ(Directory.GetPath(), "/Project/Chosen/Robot");

	Directory.SuggestPath(
		Directory.MakeSuggestedPath("Vehicle", "/Project/Scenes/"));
	EXPECT_EQ(Directory.GetPath(), "/Project/Chosen/Vehicle");

	ASSERT_TRUE(Directory.SetPath("/Project/ManualScene"));
	Directory.SuggestPath(
		Directory.MakeSuggestedPath("Character", "/Project/Scenes/"));
	EXPECT_EQ(Directory.GetPath(), "/Project/ManualScene");
}

TEST(FImportDialogDestinationModelTests, DelegatesValidationToAssetDestination)
{
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "ImportDialogState";
	std::filesystem::create_directories(Root / "Project/Content");
	const std::array Definitions{
		PathUtilities::FMountPoint{
			.VirtualRoot = "/Project/",
			.Owner = PathUtilities::EMountOwner::ActiveProject,
			.OwnerRoot = Root / "Project",
			.ContentRoot = Root / "Project/Content"}};
	const PathUtilities::FScopedMountRegistryFixture Registry(Definitions);
	ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();

	FImportDialogDestinationModel Destination;
	Destination.Reset();
	ASSERT_TRUE(Destination.SetPath("/Project/Textures/Stone"));
	const FAssetDestinationValidation Validation =
		Destination.Inspect(EmptyOccupancy);

	ASSERT_TRUE(Validation) << Validation.Message;
	EXPECT_EQ(Validation.AssetPath.ToString(), "/Project/Textures/Stone");
	EXPECT_EQ(Validation.PhysicalPath.lexically_normal(),
		(Root / "Project/Content/Textures/Stone.dasset").lexically_normal());
}
