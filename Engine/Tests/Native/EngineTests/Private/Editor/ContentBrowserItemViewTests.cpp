#include "Panels/ContentBrowserItemView.h"
#include "Settings/LevelEditorSessionSettings.h"

#include "AssetTools.h"
#include "EngineTestSupport.h"
#include "Icons/FontAwesomeIcons.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Texture/TextureCube.h"

#include <gtest/gtest.h>

namespace Durin::Editor::Level
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
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(1536), "1.5 KB");
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
			PathUtilities::FMountPoint{
				.VirtualRoot = "/ContentBrowserTextureCubeDetails/",
				.Owner = PathUtilities::EMountOwner::Test,
				.Root = Root / "Content",
				.bAutoScan = true,
				.bAuthoringWritable = true}};
		PathUtilities::FScopedMountRegistryFixture Registry(Definitions);
		ASSERT_TRUE(Registry.IsValid()) << Registry.GetError();
		FAssetPath CubePath;
		ASSERT_TRUE(FAssetPath::TryCreate(
			"/ContentBrowserTextureCubeDetails/Sky", CubePath));
		DTextureCube* Cube = nullptr;
		ASSERT_TRUE(Asset::CreateAsset(CubePath, Cube));
		ASSERT_TRUE(Asset::SavePackage(Cube->GetPackage()));
		const std::filesystem::path PackagePath = Root / "Content/Sky.dasset";
		ASSERT_TRUE(Asset::UnloadPackage(CubePath));
		ASSERT_EQ(Asset::FindResidentPackage(CubePath), nullptr);

		ContentBrowserItemView::FTextureCubeDetailsCache Cache;
		const ContentBrowserItemView::FTextureCubeDetailsSnapshot& Details =
			Cache.Get(PackagePath.generic_string(),
				Asset::GetAssetCatalogRevision());
		EXPECT_TRUE(Details.bAvailable);
		EXPECT_EQ(Details.SourceLayout, "Six Faces");
		EXPECT_EQ(Details.Source, "-");
		EXPECT_EQ(Details.SourceSize, "-");
		EXPECT_EQ(Details.Dimensions, "-");
		EXPECT_TRUE(Details.BuildDiagnostic.find("not serialized")
			!= std::string::npos);
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
				FLevelEditorSessionSettings::MinimumContentBrowserIconSize);
		const ContentBrowserItemView::FGridMetrics Default =
			ContentBrowserItemView::FGridMetrics::FromPreviewExtent(
				FLevelEditorSessionSettings::DefaultContentBrowserIconSize);
		const ContentBrowserItemView::FGridMetrics Maximum =
			ContentBrowserItemView::FGridMetrics::FromPreviewExtent(
				FLevelEditorSessionSettings::MaximumContentBrowserIconSize);

		EXPECT_LT(Minimum.CellWidth, Default.CellWidth);
		EXPECT_LT(Default.CellWidth, Maximum.CellWidth);
		EXPECT_LT(Minimum.TileHeight, Default.TileHeight);
		EXPECT_LT(Default.TileHeight, Maximum.TileHeight);
		EXPECT_FLOAT_EQ(
			Minimum.PreviewExtent,
			FLevelEditorSessionSettings::MinimumContentBrowserIconSize);
		EXPECT_FLOAT_EQ(
			Default.PreviewExtent,
			FLevelEditorSessionSettings::DefaultContentBrowserIconSize);
		EXPECT_FLOAT_EQ(
			Maximum.PreviewExtent,
			FLevelEditorSessionSettings::MaximumContentBrowserIconSize);

		ImGui::DestroyContext(Context);
	}
} // namespace Durin::Editor::Level
