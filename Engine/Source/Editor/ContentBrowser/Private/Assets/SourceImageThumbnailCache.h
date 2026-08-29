#pragma once

#include "Thumbnail/ThumbnailManager.h"
#include "Threading/Task.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Supplies source-image renderer input while keeping public lookup keyed by the requesting item.
	struct FSourceImageThumbnailRequest
	{
		std::string_view Identity;
		std::string_view PhysicalPath;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		::Durin::Editor::EAssetThumbnailPriority Priority = ::Durin::Editor::EAssetThumbnailPriority::Prefetch;
	};

	// Coordinates asynchronous decode, GPU upload, memory eviction, and disk reuse.
	class FSourceImageThumbnailCache
	{
	public:
		// Test/local caches may be unscoped; production ContentBrowser construction
		// supplies its module-owned task scope.
		FSourceImageThumbnailCache();
		explicit FSourceImageThumbnailCache(FTaskScopeToken TaskScope);
		~FSourceImageThumbnailCache();

		FSourceImageThumbnailCache(const FSourceImageThumbnailCache&) = delete;
		FSourceImageThumbnailCache& operator=(const FSourceImageThumbnailCache&) = delete;

		auto BeginFrame() -> void;
		auto Request(const FSourceImageThumbnailRequest& Request) -> void;
		auto Find(std::string_view Identity) const -> ::Durin::Editor::FAssetThumbnailView;
		auto EndFrame() -> void;
		auto CancelPendingRequests() -> void;
		auto Clear() -> void;
		auto Shutdown() -> void;
		auto IsShuttingDown() const -> bool;
		auto GetTrackedTaskCountForTesting() const -> size_t;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin::Editor::ContentBrowser::Private
