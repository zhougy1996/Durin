#include "Editor/ContentBrowserItemTestSupport.h"
#include "Editor/ContentBrowserQueryTestSupport.h"
#include "Panels/ContentBrowserItemView.h"
#include "ContentBrowser/ContentBrowserTool.h"
#include "Icons/FontAwesomeIcons.h"
#include "Editor/BrowserDefinitionTestSupport.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

using Durin::ContentBrowser::Private::TestKind;
using Durin::ContentBrowser::Private::TestName;
using Durin::ContentBrowser::Private::TestVirtualPath;
using Durin::ContentBrowser::Private::TestPhysicalPath;
using Durin::ContentBrowser::Private::TestPackagePath;
using Durin::ContentBrowser::Private::TestAssetClassName;
using Durin::ContentBrowser::Private::TestExtension;
using Durin::ContentBrowser::Private::TestRedirectDestination;
using Durin::ContentBrowser::Private::TestFileSize;
using Durin::ContentBrowser::Private::TestPackageFileSize;
using Durin::ContentBrowser::Private::TestBulkFileSize;
using Durin::ContentBrowser::Private::TestLastWriteTime;

namespace Durin::ContentBrowser::Private
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
		Testing::InitializeDObjectSystemForTests();
		auto* Definition = NewObject<FBrowserTestDefinition>(nullptr, {});
		Definition->ClassPath = "ItemViewTest::DTextureCube"; Definition->DisplayName = "Texture Cube";
		Definition->Icon = Icons::Cube; Definition->Categories = {{"Texture"}};
		auto Registration = FAssetDefinitionRegistry::Get().RegisterAssetDefinition(*Definition);
		ASSERT_TRUE(Registration);
		FContentBrowserItem Folder = Durin::ContentBrowser::Private::FContentBrowserNativeItem{
			.Kind = EContentBrowserItemKind::Folder,
			.Name = "Textures"};
		FContentBrowserItem Asset = Durin::ContentBrowser::Private::FContentBrowserNativeItem{
			.Kind = EContentBrowserItemKind::Asset,
			.Name = "Sky",
			.AssetClassName = "ItemViewTest::DTextureCube"};
		FContentBrowserItem Source = Durin::ContentBrowser::Private::FContentBrowserNativeItem{
			.Kind = EContentBrowserItemKind::File,
			.Name = "Sky.hdr",
			.Extension = ".hdr"};
		FContentBrowserItem Redirector = Durin::ContentBrowser::Private::FContentBrowserNativeItem{
			.Kind = EContentBrowserItemKind::Redirector,
			.Name = "OldSky"};

		Durin::ContentBrowser::Private::FNativeContentBrowserTestQuery Query;
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Query.BindItemForTesting(Folder)), "Folder");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Query.BindItemForTesting(Asset)), "Texture Cube");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Query.BindItemForTesting(Source)), "hdr file");
		EXPECT_EQ(ContentBrowserItemView::TypeLabel(Query.BindItemForTesting(Redirector)), "Redirector");
		EXPECT_EQ(ContentBrowserItemView::Icon(Query.BindItemForTesting(Redirector)), Icons::ArrowRight);
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(512), "512 B");
		EXPECT_EQ(ContentBrowserItemView::FormatFileSize(1536), "1.50 KiB");
	}

	TEST(FContentBrowserItemViewTests, DerivesMonotonicMetricsAtSupportedSizes)
	{
		ImGuiContext* Context = ImGui::CreateContext();
		ASSERT_NE(Context, nullptr);

		const ContentBrowserItemView::FGridMetrics Minimum =
			ContentBrowserItemView::FGridMetrics::FromPreviewExtent(
				::Durin::ContentBrowser::FPresentationSettings::MinimumIconSize);
		const ContentBrowserItemView::FGridMetrics Default =
			ContentBrowserItemView::FGridMetrics::FromPreviewExtent(
				::Durin::ContentBrowser::FPresentationSettings::DefaultIconSize);
		const ContentBrowserItemView::FGridMetrics Maximum =
			ContentBrowserItemView::FGridMetrics::FromPreviewExtent(
				::Durin::ContentBrowser::FPresentationSettings::MaximumIconSize);

		EXPECT_LT(Minimum.CellWidth, Default.CellWidth);
		EXPECT_LT(Default.CellWidth, Maximum.CellWidth);
		EXPECT_LT(Minimum.TileHeight, Default.TileHeight);
		EXPECT_LT(Default.TileHeight, Maximum.TileHeight);
		EXPECT_FLOAT_EQ(
			Minimum.PreviewExtent,
			::Durin::ContentBrowser::FPresentationSettings::MinimumIconSize);
		EXPECT_FLOAT_EQ(
			Default.PreviewExtent,
			::Durin::ContentBrowser::FPresentationSettings::DefaultIconSize);
		EXPECT_FLOAT_EQ(
			Maximum.PreviewExtent,
			::Durin::ContentBrowser::FPresentationSettings::MaximumIconSize);

		ImGui::DestroyContext(Context);
	}
} // namespace Durin::ContentBrowser::Private
