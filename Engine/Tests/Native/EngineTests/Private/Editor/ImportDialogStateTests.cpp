#include "Assets/ImportDialogState.h"

#include "EngineTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::Editor::Level;

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
			.Root = Root / "Project/Content",
			.bAutoScan = true}};
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

TEST(FMountedSourceImportFormModelTests, PreservesManualDestinationAndResetsMode)
{
	Durin::Editor::Level::FMountedSourceImportFormModel Model;
	Model.Reset();
	EXPECT_EQ(Model.GetMode(),
		Durin::Editor::Level::EMountedSourceImportMode::IngestExternal);
	Model.SuggestDestination("/Project/Sources/Textures/First.png");
	EXPECT_STREQ(Model.GetDestinationBuffer().data(),
		"/Project/Sources/Textures/First.png");
	Model.SuggestDestination("/Project/Sources/Textures/Second.png");
	EXPECT_STREQ(Model.GetDestinationBuffer().data(),
		"/Project/Sources/Textures/Second.png");
	ASSERT_TRUE(Model.SetDestination("/Project/Sources/Manual.png"));
	Model.SuggestDestination("/Project/Sources/Textures/Third.png");
	EXPECT_STREQ(Model.GetDestinationBuffer().data(),
		"/Project/Sources/Manual.png");
	Model.GetMode() = Durin::Editor::Level::EMountedSourceImportMode::ReferenceExisting;
	Model.Reset();
	EXPECT_EQ(Model.GetMode(),
		Durin::Editor::Level::EMountedSourceImportMode::IngestExternal);
	EXPECT_EQ(Model.GetDestinationBuffer()[0], '\0');
}

TEST(FTextureImportDialogStateTests, DefaultsToTexture2DAndResetsEveryForm)
{
	FTextureImportDialogState State;
	State.Reset();
	EXPECT_EQ(State.GetAssetType(), ETextureImportAssetType::Texture2D);
	EXPECT_EQ(State.GetSourceMode(),
		EMountedSourceImportMode::IngestExternal);
	EXPECT_EQ(State.GetTextureCube().SourceLayout,
		ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(State.GetVolumeTexture().SliceWidth, 128u);
	EXPECT_EQ(State.GetVolumeTexture().Depth, 128u);

	State.SetAssetType(ETextureImportAssetType::VolumeTexture);
	State.SetSourceMode(EMountedSourceImportMode::ReferenceExisting);
	State.GetTexture2D().Source.GetSourcePathBuffer()[0] = '2';
	State.GetTextureCube().PanoramaPathBuffer[0] = 'c';
	State.GetTextureCube().SourceLayout = ETextureCubeSourceLayout::SixFaces;
	State.GetVolumeTexture().Source.GetSourcePathBuffer()[0] = 'v';
	State.GetVolumeTexture().Depth = 17;

	State.Reset();
	EXPECT_EQ(State.GetAssetType(), ETextureImportAssetType::Texture2D);
	EXPECT_EQ(State.GetSourceMode(),
		EMountedSourceImportMode::IngestExternal);
	EXPECT_EQ(State.GetTexture2D().Source.GetSourcePathBuffer()[0], '\0');
	EXPECT_EQ(State.GetTextureCube().PanoramaPathBuffer[0], '\0');
	EXPECT_EQ(State.GetTextureCube().SourceLayout,
		ETextureCubeSourceLayout::EquirectangularPanorama);
	EXPECT_EQ(State.GetVolumeTexture().Source.GetSourcePathBuffer()[0], '\0');
	EXPECT_EQ(State.GetVolumeTexture().Depth, 128u);
}

TEST(FTextureImportDialogStateTests, PreservesInactiveFormsAcrossTypeSwitches)
{
	FTextureImportDialogState State;
	State.Reset();
	State.GetTexture2D().Source.GetSourcePathBuffer()[0] = '2';
	State.GetTextureCube().PanoramaPathBuffer[0] = 'p';
	State.GetTextureCube().FacePathBuffers[0][0] = 'f';
	State.GetTextureCube().SourceLayout = ETextureCubeSourceLayout::SixFaces;
	State.GetVolumeTexture().Source.GetSourcePathBuffer()[0] = 'v';
	State.GetVolumeTexture().TilesX = 7;

	State.SetAssetType(ETextureImportAssetType::TextureCube);
	State.SetAssetType(ETextureImportAssetType::VolumeTexture);
	State.SetAssetType(ETextureImportAssetType::Texture2D);

	EXPECT_EQ(State.GetTexture2D().Source.GetSourcePathBuffer()[0], '2');
	EXPECT_EQ(State.GetTextureCube().PanoramaPathBuffer[0], 'p');
	EXPECT_EQ(State.GetTextureCube().FacePathBuffers[0][0], 'f');
	EXPECT_EQ(State.GetTextureCube().SourceLayout,
		ETextureCubeSourceLayout::SixFaces);
	EXPECT_EQ(State.GetVolumeTexture().Source.GetSourcePathBuffer()[0], 'v');
	EXPECT_EQ(State.GetVolumeTexture().TilesX, 7u);
}

TEST(FMeshCoordinateImportModelTests, AppliesSharedPresets)
{
	Durin::Editor::Level::FMeshCoordinateImportModel Model;
	Model.SetPreset(Durin::Editor::Level::FMeshCoordinateImportModel::EPreset::
		YUpNegativeZForward);
	const Durin::FStaticMeshImportSettings Expected =
		Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	EXPECT_EQ(Model.GetSettings().ForwardAxis, Expected.ForwardAxis);
	EXPECT_EQ(Model.GetSettings().RightAxis, Expected.RightAxis);
	EXPECT_EQ(Model.GetSettings().UpAxis, Expected.UpAxis);
	Model.Reset();
	EXPECT_EQ(Model.GetSettings().ForwardAxis,
		Durin::FStaticMeshImportSettings::MakeDurin().ForwardAxis);
}
