#include "Panels/ContentBrowserItemView.h"
#include "Settings/LevelEditorSessionSettings.h"

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "Asset/AssetCook.h"
#include "EngineTestSupport.h"
#include "Icons/FontAwesomeIcons.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Texture/TextureCube.h"
#include "AssetForge/Builtins/TextureCubeImport.h"
#include "Texture/TextureCubeFactoryTestSupport.h"

#include <gtest/gtest.h>

#include "ContentBrowser/ContentBrowserTool.h"

namespace Durin::Editor::ContentBrowser::Private
{
	TEST(FContentBrowserItemViewTests, MapsProviderStatesWithoutOwningTextures)
	{
		Editor::FAssetThumbnailView Thumbnail;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Icon);

		Thumbnail.State = Editor::EAssetThumbnailState::Queued;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Loading);
		Thumbnail.State = Editor::EAssetThumbnailState::Loading;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Loading);
		Thumbnail.State = Editor::EAssetThumbnailState::Uploading;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Loading);

		Thumbnail.State = Editor::EAssetThumbnailState::Ready;
		Thumbnail.Texture = reinterpret_cast<FRHITexture*>(1);
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Ready);

		Thumbnail.State = Editor::EAssetThumbnailState::Failed;
		Thumbnail.Texture = nullptr;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Failed);

		Thumbnail.State = Editor::EAssetThumbnailState::Invalid;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Icon);
	}

	TEST(FContentBrowserItemViewTests, PreservesMixedItemLabelsAndFormatting)
	{
		FContentBrowserItem Folder{
			.Kind = EContentBrowserItemKind::Folder,
			.Name = "Textures"};
		FContentBrowserItem Asset{
			.Kind = EContentBrowserItemKind::Asset,
			.Name = "Sky",
			.AssetClassName = "Durin::DTextureCube"};
		FContentBrowserItem Source{
			.Kind = EContentBrowserItemKind::File,
			.Name = "Sky.hdr",
			.Extension = ".hdr"};
		FContentBrowserItem Redirector{
			.Kind = EContentBrowserItemKind::Redirector,
			.Name = "OldSky"};

		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Folder), "Folder");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Asset), "Texture Cube");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Source), "hdr file");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Redirector), "Redirector");
		EXPECT_STREQ(ContentBrowserItemView::Icon(Redirector), Icons::ArrowRight);
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(512), "512 B");
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(1536), "1.50 KiB");
	}

	TEST(FContentBrowserItemViewTests, InspectsTextureCubeDetailsWithoutLoadingPackage)
	{
		InitializeDObjectSystem();
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
		ASSERT_TRUE(Asset::UnloadPackage(CubePath));
		ASSERT_EQ(Asset::FindResidentPackage(CubePath), nullptr);

		ContentBrowserItemView::FTextureCubeDetailsCache Cache;
		const ContentBrowserItemView::FTextureCubeDetailsSnapshot& Details =
			Cache.Get(PackagePath.generic_string(),
				Asset::GetAssetCatalogRevision());
		EXPECT_TRUE(Details.bAvailable);
		EXPECT_EQ(Details.SourceLayout, "Six Faces");
		EXPECT_EQ(Details.Source, "6 of 6 face sources");
		EXPECT_NE(Details.SourceSize, "-");
		EXPECT_EQ(Details.Dimensions, "-");
		EXPECT_FALSE(Details.BuildDiagnostic.empty());
		EXPECT_EQ(Asset::FindResidentPackage(CubePath), nullptr);
		Cache.Get(PackagePath.generic_string(),
			Asset::GetAssetCatalogRevision());
		EXPECT_EQ(Asset::FindResidentPackage(CubePath), nullptr);
	}

	TEST(FContentBrowserItemViewTests, InvalidatesTextureCubeDetailsCache)
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
		ContentBrowserItemView::FTextureCubeDetailsCache Cache(
			[&](std::string_view) {
				ContentBrowserItemView::FTextureCubeDetailsSnapshot Snapshot;
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

	TEST(FContentBrowserItemViewTests, ReportsUnavailableAndCorruptTextureCubeMetadata)
	{
		const std::filesystem::path Root =
			Testing::GetTestWorkDirectory() / "ContentBrowserTextureCubeInvalid";
		std::error_code Error;
		Testing::RemoveTestWorkDirectory(Root, Error);
		std::filesystem::create_directories(Root);
		const std::filesystem::path Missing = Root / "Missing.dasset";
		const ContentBrowserItemView::FTextureCubeDetailsSnapshot Unavailable =
			ContentBrowserItemView::BuildTextureCubeDetailsSnapshot(
				Missing.generic_string());
		EXPECT_FALSE(Unavailable.bAvailable);
		EXPECT_FALSE(Unavailable.BuildDiagnostic.empty());

		const std::filesystem::path Corrupt = Root / "Corrupt.dasset";
		{
			std::ofstream File(Corrupt, std::ios::binary);
			File << "not a package";
		}
		const ContentBrowserItemView::FTextureCubeDetailsSnapshot Invalid =
			ContentBrowserItemView::BuildTextureCubeDetailsSnapshot(
				Corrupt.generic_string());
		EXPECT_FALSE(Invalid.bAvailable);
		EXPECT_FALSE(Invalid.BuildDiagnostic.empty());
	}

	TEST(FContentBrowserItemViewTests, DerivesMonotonicMetricsAtSupportedSizes)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);

		const ContentBrowserItemView::FGridMetrics Minimum =
			ContentBrowserItemView::FGridMetrics::FromPreviewExtent(
				::Durin::Editor::ContentBrowser::FPresentationSettings::MinimumIconSize);
		const ContentBrowserItemView::FGridMetrics Default =
			ContentBrowserItemView::FGridMetrics::FromPreviewExtent(
				::Durin::Editor::ContentBrowser::FPresentationSettings::DefaultIconSize);
		const ContentBrowserItemView::FGridMetrics Maximum =
			ContentBrowserItemView::FGridMetrics::FromPreviewExtent(
				::Durin::Editor::ContentBrowser::FPresentationSettings::MaximumIconSize);

		EXPECT_LT(Minimum.CellWidth, Default.CellWidth);
		EXPECT_LT(Default.CellWidth, Maximum.CellWidth);
		EXPECT_LT(Minimum.TileHeight, Default.TileHeight);
		EXPECT_LT(Default.TileHeight, Maximum.TileHeight);
		EXPECT_FLOAT_EQ(
			Minimum.PreviewExtent,
			::Durin::Editor::ContentBrowser::FPresentationSettings::MinimumIconSize);
		EXPECT_FLOAT_EQ(
			Default.PreviewExtent,
			::Durin::Editor::ContentBrowser::FPresentationSettings::DefaultIconSize);
		EXPECT_FLOAT_EQ(
			Maximum.PreviewExtent,
			::Durin::Editor::ContentBrowser::FPresentationSettings::MaximumIconSize);

		ImGui::DestroyContext(Context);
	}
} // namespace Durin::Editor::ContentBrowser::Private
