#pragma once

#include "Panels/ContentBrowserModel.h"
#include "Thumbnail/ThumbnailManager.h"

#include "MonaImGui.h"

namespace Durin::Editor::ContentBrowser::Private::ContentBrowserItemView
{
	// Describes the visual fallback selected for a thumbnail renderer state.
	enum class EThumbnailPresentation : uint8
	{
		Icon,
		Loading,
		Ready,
		Failed
	};

	// Derives every grid-card region from the requested square preview extent.
	struct FGridMetrics
	{
		float CellWidth = 0.0f;
		float TileHeight = 0.0f;
		float RowHeight = 0.0f;
		float PreviewExtent = 0.0f;
		float IconFontSize = 0.0f;
		float NameFontSize = 0.0f;
		float CardInset = 0.0f;
		float CardRounding = 0.0f;
		float ContentInset = 0.0f;
		float NameGap = 0.0f;
		float NameAreaHeight = 0.0f;
		float BadgeSize = 0.0f;
		float BadgeInset = 0.0f;

		static auto FromPreviewExtent(float PreviewExtent) -> FGridMetrics;
		auto CardMin(const ImVec2& TileStart) const -> ImVec2;
		auto CardMax(const ImVec2& TileStart, const ImVec2& TileSize) const -> ImVec2;
		auto PreviewMin(const ImVec2& TileStart, const ImVec2& TileSize) const -> ImVec2;
		auto PreviewMax(const ImVec2& TileStart, const ImVec2& TileSize) const -> ImVec2;
		auto NameMin(const ImVec2& TileStart, const ImVec2& TileSize) const -> ImVec2;
		auto NameMax(const ImVec2& TileStart, const ImVec2& TileSize) const -> ImVec2;
	};

	auto ResolveThumbnailPresentation(const ::Durin::Editor::FAssetThumbnailView& Thumbnail)
		-> EThumbnailPresentation;
	auto TypeLabel(const FContentBrowserItem& Item) -> std::string;
	auto Icon(const FContentBrowserItem& Item) -> std::string;
	auto FormatFileSize(uintmax_t Bytes) -> std::string;
	auto FormatFileTime(const std::filesystem::file_time_type& Time) -> std::string;
	auto DrawTransparencyGrid(ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Size) -> void;
	auto DrawThumbnailBadge(
		ImDrawList& DrawList,
		const FGridMetrics& Metrics,
		const ImVec2& PreviewMax, const char* BadgeIcon) -> void;
	auto DrawThumbnail(
		const FContentBrowserItem& Item,
		const ::Durin::Editor::FAssetThumbnailView& Thumbnail,
		const FGridMetrics& Metrics,
		const ImVec2& PreviewMin,
		const ImVec2& PreviewMax,
		const ImVec2& CursorAfterTile) -> bool;
	auto DrawFallbackIcon(
		const FContentBrowserItem& Item,
		EThumbnailPresentation ThumbnailPresentation,
		const FGridMetrics& Metrics,
		const ImVec2& PreviewMin,
		const ImVec2& PreviewMax) -> void;
	auto DrawLabel(
		const FContentBrowserItem& Item,
		const FGridMetrics& Metrics,
		const ImVec2& NameMin,
		const ImVec2& NameMax) -> void;
} // namespace Durin::Editor::ContentBrowser::Private::ContentBrowserItemView
