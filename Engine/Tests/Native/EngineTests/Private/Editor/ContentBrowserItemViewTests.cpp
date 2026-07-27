#include "Panels/ContentBrowserItemView.h"
#include "Settings/LevelEditorSessionSettings.h"

#include <gtest/gtest.h>

namespace Durin
{
	TEST(FContentBrowserItemViewTests, MapsProviderStatesWithoutOwningTextures)
	{
		FAssetThumbnailView Thumbnail;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Icon);

		Thumbnail.State = EAssetThumbnailState::Queued;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Loading);
		Thumbnail.State = EAssetThumbnailState::Loading;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Loading);
		Thumbnail.State = EAssetThumbnailState::Uploading;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Loading);

		Thumbnail.State = EAssetThumbnailState::Ready;
		Thumbnail.Texture = reinterpret_cast<FRHITexture*>(1);
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Ready);

		Thumbnail.State = EAssetThumbnailState::Failed;
		Thumbnail.Texture = nullptr;
		EXPECT_EQ(
			ContentBrowserItemView::ResolveThumbnailPresentation(Thumbnail),
			ContentBrowserItemView::EThumbnailPresentation::Failed);

		Thumbnail.State = EAssetThumbnailState::Invalid;
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
			.Kind = EContentBrowserItemKind::SourceFile,
			.Name = "Sky.hdr",
			.Extension = ".hdr"};

		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Folder), "Folder");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Asset), "Texture Cube");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Source), "hdr file");
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(512), "512 B");
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(1536), "1.5 KB");
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
} // namespace Durin
