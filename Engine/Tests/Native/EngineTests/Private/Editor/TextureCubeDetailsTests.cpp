#include "ContentBrowser/TextureCubeDetails.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "EngineTestSupport.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "Texture/TextureCube.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"

#include <gtest/gtest.h>


namespace Durin::Editor::Texture
{
	TEST(FTextureCubeDetailsTests, InspectsTextureCubeDetailsWithoutLoadingPackage)
	{
		InitializeDObjectSystem();
		FModuleManager::Get().LoadModuleChecked("TextureBuild");
		const std::filesystem::path Root =
			Testing::GetTestWorkDirectory() / "ContentBrowserTextureCubeDetails";
		std::error_code Error;
		Testing::RemoveTestWorkDirectory(Root, Error);
		std::filesystem::create_directories(Root / "Content");
		const std::array Definitions{
			FMountPoint{
				.VirtualRoot = "/ContentBrowserTextureCubeDetails/",
				.Owner = EMountOwner::Test,
				.Root = Root / "Content",
				.bAutoScan = true,
				.bContentWritable = true}};
		Testing::FScopedMountRegistryFixture Registry(Definitions);
		ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
		FPackagePath CubePath;
		ASSERT_TRUE(FPackagePath::TryCreate(
			"/ContentBrowserTextureCubeDetails/Sky", CubePath));
		constexpr std::array<std::string_view, TextureCubeFaceCount> FaceNames{
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		std::array<std::string, TextureCubeFaceCount> FaceFiles;
		for (size_t Index = 0; Index < FaceFiles.size(); ++Index)
			FaceFiles[Index] = (std::filesystem::path(DURIN_TEST_DATA_DIR)
				/ "SkyBoxConvention" / std::format("{}.png", FaceNames[Index])).generic_string();
		const Durin::Testing::TFactoryImportResult<Durin::DTextureCube> Imported =
			AssetForge::Builtins::ImportTextureCubeFacesForTest(
				FaceFiles, CubePath.ToString());
		ASSERT_TRUE(Imported) << Imported.Message;
		const std::filesystem::path PackagePath = Root / "Content/Sky.dasset";
		ASSERT_TRUE(UnloadPackage(CubePath));
		ASSERT_EQ(FindResidentPackage(CubePath), nullptr);

		Editor::Texture::FTextureCubeDetailsCache Cache;
		const Editor::Texture::FTextureCubeDetailsSnapshot& Details =
			Cache.Get(PackagePath.generic_string(),
				GetAssetCatalogRevision());
		EXPECT_TRUE(Details.bAvailable);
		EXPECT_EQ(Details.SourceLayout, "Six Faces");
		EXPECT_EQ(Details.Source, "6 of 6 face sources");
		EXPECT_NE(Details.SourceSize, "-");
		EXPECT_EQ(Details.Dimensions, "-");
		EXPECT_FALSE(Details.BuildDiagnostic.empty());
		EXPECT_EQ(FindResidentPackage(CubePath), nullptr);
		std::string PresentationError;
		auto Presentation = ContentBrowser::RegisterAssetTypePresentation({
			.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.DisplayName = "Cube", .Category = ContentBrowser::EAssetCategory::Texture, .Icon = "cube",
			.Details = MakeTextureCubeDetailsProvider()}, PresentationError);
		ASSERT_TRUE(Presentation.IsValid()) << PresentationError;
		ContentBrowser::FExtensionContext Context;
		Context.CatalogRevision = GetAssetCatalogRevision();
		Context.Selection.push_back({.Kind = ContentBrowser::EExtensionItemKind::Asset,
			.PhysicalPath = PackagePath.generic_string(),
			.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString()});
		const auto Rows = ContentBrowser::CaptureSelectionDetails(Context);
		const auto Source = std::ranges::find(Rows, "Source", &ContentBrowser::FDetailRow::Label);
		ASSERT_NE(Source, Rows.end());
		EXPECT_EQ(Source->Value, "6 of 6 face sources");
		EXPECT_EQ(FindResidentPackage(CubePath), nullptr);
		Cache.Get(PackagePath.generic_string(),
			GetAssetCatalogRevision());
		EXPECT_EQ(FindResidentPackage(CubePath), nullptr);
	}

	TEST(FTextureCubeDetailsTests, InvalidatesTextureCubeDetailsCache)
	{
		const std::filesystem::path Root =
			Testing::GetTestWorkDirectory() / "ContentBrowserTextureCubeCache";
		std::error_code Error;
		Testing::RemoveTestWorkDirectory(Root, Error);
		std::filesystem::create_directories(Root);
		const std::filesystem::path PackagePath = Root / "Sky.dasset";
		{
			std::ofstream File(PackagePath);
			File << "a";
		}
		size_t BuildCount = 0;
		Editor::Texture::FTextureCubeDetailsCache Cache(
			[&](std::string_view) {
				Editor::Texture::FTextureCubeDetailsSnapshot Snapshot;
				Snapshot.BuildDiagnostic = std::format("build {}", ++BuildCount);
				return Snapshot;
			});
		EXPECT_EQ(Cache.Get(PackagePath.generic_string(), 10).BuildDiagnostic, "build 1");
		EXPECT_EQ(Cache.Get(PackagePath.generic_string(), 10).BuildDiagnostic, "build 1");
		EXPECT_EQ(BuildCount, 1);
		{
			std::ofstream File(PackagePath, std::ios::trunc);
			File << "larger package";
		}
		EXPECT_EQ(Cache.Get(PackagePath.generic_string(), 10).BuildDiagnostic, "build 2");
		EXPECT_EQ(Cache.Get(PackagePath.generic_string(), 11).BuildDiagnostic, "build 3");
		Cache.Invalidate();
		EXPECT_EQ(Cache.Get(PackagePath.generic_string(), 11).BuildDiagnostic, "build 4");
	}

	TEST(FTextureCubeDetailsTests, ReportsUnavailableAndCorruptTextureCubeMetadata)
	{
		const std::filesystem::path Root =
			Testing::GetTestWorkDirectory() / "ContentBrowserTextureCubeInvalid";
		std::error_code Error;
		Testing::RemoveTestWorkDirectory(Root, Error);
		std::filesystem::create_directories(Root);
		const std::filesystem::path Missing = Root / "Missing.dasset";
		const Editor::Texture::FTextureCubeDetailsSnapshot Unavailable =
			Editor::Texture::BuildTextureCubeDetailsSnapshot(
				Missing.generic_string());
		EXPECT_FALSE(Unavailable.bAvailable);
		EXPECT_FALSE(Unavailable.BuildDiagnostic.empty());

		const std::filesystem::path Corrupt = Root / "Corrupt.dasset";
		{
			std::ofstream File(Corrupt, std::ios::binary);
			File << "not a package";
		}
		const Editor::Texture::FTextureCubeDetailsSnapshot Invalid =
			Editor::Texture::BuildTextureCubeDetailsSnapshot(
				Corrupt.generic_string());
		EXPECT_FALSE(Invalid.bAvailable);
		EXPECT_FALSE(Invalid.BuildDiagnostic.empty());
	}

}
