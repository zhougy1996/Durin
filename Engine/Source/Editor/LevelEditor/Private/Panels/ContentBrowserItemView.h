#pragma once

#include "Panels/ContentBrowserModel.h"
#include "Thumbnail/AssetThumbnailProvider.h"

#include "MonaImGui.h"

namespace Durin::Editor::Level::ContentBrowserItemView
{
	// Describes the visual fallback selected for a thumbnail provider state.
	enum class EThumbnailPresentation : uint8
	{
		Icon,
		Loading,
		Ready,
		Failed
	};

	struct FTextureCubeDetailsSnapshot
	{
		bool bAvailable = false;
		bool bPanorama = false;
		std::string SourceLayout = "-";
		std::string Source = "-";
		std::string SourceSize = "-";
		std::string FaceOverride = "-";
		std::string InputRange = "-";
		std::string Exposure = "-";
		std::string Dimensions = "-";
		std::string Faces = "-";
		std::string Mips = "-";
		std::string Output = "-";
		std::string BuildDiagnostic = "Metadata is unavailable.";
		uint64 PackageHashLow = 0;
		uint64 PackageHashHigh = 0;
	};

	class FTextureCubeDetailsCache
	{
	public:
		using FBuilder = std::function<FTextureCubeDetailsSnapshot(std::string_view)>;

		explicit FTextureCubeDetailsCache(FBuilder InBuilder = {});
		auto Get(std::string_view PhysicalPath, uint64 RegistryRevision)
			-> const FTextureCubeDetailsSnapshot&;
		auto Invalidate() -> void;

	private:
		FBuilder Builder;
		std::string CachedPhysicalPath;
		uint64 CachedRegistryRevision = std::numeric_limits<uint64>::max();
		uintmax_t CachedFileSize = 0;
		int64 CachedLastWriteTimeTicks = 0;
		bool bCachedFileStatValid = false;
		std::optional<FTextureCubeDetailsSnapshot> CachedSnapshot;
	};

	auto BuildTextureCubeDetailsSnapshot(std::string_view PhysicalPath)
		-> FTextureCubeDetailsSnapshot;

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
	auto Icon(const FContentBrowserItem& Item) -> const char*;
	auto FormatFileSize(uintmax_t Bytes) -> std::string;
	auto FormatFileTime(const std::filesystem::file_time_type& Time) -> std::string;
	auto DrawTransparencyGrid(ImDrawList& DrawList, const ImVec2& Min, const ImVec2& Size) -> void;
	auto DrawTextureCubeBadge(
		ImDrawList& DrawList,
		const FGridMetrics& Metrics,
		const ImVec2& PreviewMax) -> void;
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
} // namespace Durin::Editor::Level::ContentBrowserItemView
