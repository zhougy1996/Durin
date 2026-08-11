#pragma once

#include "Thumbnail/AssetThumbnailProvider.h"

namespace Durin
{
	namespace Editor { class FRenderedAssetThumbnailCache; }
	class FSourceImageThumbnailCache;

	// Supplies either source-image input or an authored asset fingerprint through one card request.
	struct FContentBrowserThumbnailRequest
	{
		std::string_view Identity;
		std::string_view SourcePhysicalPath;
		uintmax_t SourceFileSize = 0;
		std::filesystem::file_time_type SourceLastWriteTime{};
		std::optional<Editor::FAssetThumbnailPackageFingerprint> Asset;
		Editor::EAssetThumbnailPriority Priority = Editor::EAssetThumbnailPriority::Prefetch;
	};

	// Presents source and rendered providers through one Content Browser lifecycle.
	class FContentBrowserThumbnailCache
	{
	public:
		FContentBrowserThumbnailCache();
		~FContentBrowserThumbnailCache();

		FContentBrowserThumbnailCache(const FContentBrowserThumbnailCache&) = delete;
		FContentBrowserThumbnailCache& operator=(const FContentBrowserThumbnailCache&) = delete;

		auto BeginFrame() -> void;
		auto Request(const FContentBrowserThumbnailRequest& Request) -> void;
		auto Find(std::string_view Identity) const -> Editor::FAssetThumbnailView;
		auto EndFrame() -> void;
		auto CancelPendingRequests() -> void;
		auto Clear() -> void;

	private:
		std::unique_ptr<FSourceImageThumbnailCache> SourceImages;
		std::unique_ptr<Editor::FRenderedAssetThumbnailCache> RenderedAssets;
		std::unordered_map<std::string, FAssetPath> RenderedIdentities;
	};
} // namespace Durin
