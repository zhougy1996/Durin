#include "Panels/ContentBrowserItemView.h"
#include "ContentBrowser/ContentBrowserTool.h"
#include "Icons/FontAwesomeIcons.h"

#include <gtest/gtest.h>

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
		std::string Error;
		auto Registration = RegisterAssetTypePresentation({
			.AssetClassName = "Durin::DTextureCube", .DisplayName = "Texture Cube",
			.Category = EAssetCategory::Texture, .Icon = Icons::Cube}, Error);
		ASSERT_TRUE(Registration.IsValid());
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
		EXPECT_EQ(ContentBrowserItemView::Icon(Redirector), Icons::ArrowRight);
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(512), "512 B");
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(1536), "1.50 KiB");
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
