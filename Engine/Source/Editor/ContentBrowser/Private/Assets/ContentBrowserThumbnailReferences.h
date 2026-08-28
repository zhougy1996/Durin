#pragma once

#include "AssetThumbnail.h"
#include "Threading/Task.h"

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

	// Keeps ordinary-file previews private while asset entries are represented by
	// lightweight FAssetThumbnail references into the manager-owned shared pool.
	class FContentBrowserThumbnailReferences
	{
	public:
		// Test/local caches may use the source cache's local task scope.
		FContentBrowserThumbnailReferences();
		explicit FContentBrowserThumbnailReferences(FTaskScopeToken ThumbnailTaskScope);
		~FContentBrowserThumbnailReferences();

		FContentBrowserThumbnailReferences(const FContentBrowserThumbnailReferences&) = delete;
		FContentBrowserThumbnailReferences& operator=(const FContentBrowserThumbnailReferences&) = delete;

		auto BeginFrame() -> void;
		auto Request(const FContentBrowserThumbnailRequest& Request) -> void;
		auto Find(std::string_view Identity) const -> ::Durin::Editor::FAssetThumbnailView;
		auto EndFrame() -> void;
		auto CancelPendingRequests() -> void;
		auto Clear() -> void;

	private:
		std::unique_ptr<FSourceImageThumbnailCache> SourceImages;
		std::unordered_map<std::string, std::unique_ptr<::Durin::Editor::FAssetThumbnail>> AssetThumbnails;
	};
} // namespace Durin::Editor::ContentBrowser::Private
