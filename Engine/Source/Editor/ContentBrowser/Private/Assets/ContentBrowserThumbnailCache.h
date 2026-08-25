#pragma once

#include "Thumbnail/AssetThumbnailProvider.h"
#include "Threading/Task.h"

namespace Durin::Editor { class FRenderedAssetThumbnailCache; }

namespace Durin::Editor::ContentBrowser::Private
{
	class FSourceImageThumbnailCache;

	// Supplies either source-image input or an authored asset fingerprint through one card request.
	struct FContentBrowserThumbnailRequest
	{
		std::string_view Identity;
		std::string_view SourcePhysicalPath;
		uintmax_t SourceFileSize = 0;
		std::filesystem::file_time_type SourceLastWriteTime{};
		std::optional<::Durin::Editor::FAssetThumbnailPackageFingerprint> Asset;
		::Durin::Editor::EAssetThumbnailPriority Priority = ::Durin::Editor::EAssetThumbnailPriority::Prefetch;
	};

	// Presents source and rendered providers through one Content Browser lifecycle.
	class FContentBrowserThumbnailCache
	{
	public:
		// Test/local caches may use the source cache's local task scope.
		FContentBrowserThumbnailCache();
		explicit FContentBrowserThumbnailCache(FTaskScopeToken ThumbnailTaskScope);
		~FContentBrowserThumbnailCache();

		FContentBrowserThumbnailCache(const FContentBrowserThumbnailCache&) = delete;
		FContentBrowserThumbnailCache& operator=(const FContentBrowserThumbnailCache&) = delete;

		auto BeginFrame() -> void;
		auto Request(const FContentBrowserThumbnailRequest& Request) -> void;
		auto Find(std::string_view Identity) const -> ::Durin::Editor::FAssetThumbnailView;
		auto EndFrame() -> void;
		auto CancelPendingRequests() -> void;
		auto Clear() -> void;

	private:
		std::unique_ptr<FSourceImageThumbnailCache> SourceImages;
		std::unique_ptr<::Durin::Editor::FRenderedAssetThumbnailCache> RenderedAssets;
		std::unordered_map<std::string, FAssetPath> RenderedIdentities;
	};
} // namespace Durin::Editor::ContentBrowser::Private
