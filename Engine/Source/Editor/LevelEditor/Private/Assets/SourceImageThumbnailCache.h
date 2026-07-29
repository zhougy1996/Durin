#pragma once

#include "Thumbnail/AssetThumbnail.h"

namespace Durin
{
	// Supplies source-image provider input while keeping public lookup keyed by the requesting item.
	struct FSourceImageThumbnailRequest
	{
		std::string_view Identity;
		std::string_view PhysicalPath;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
	};

	// Coordinates asynchronous decode, GPU upload, memory eviction, and disk reuse.
	class FSourceImageThumbnailCache
	{
	public:
		FSourceImageThumbnailCache();
		~FSourceImageThumbnailCache();

		FSourceImageThumbnailCache(const FSourceImageThumbnailCache&) = delete;
		FSourceImageThumbnailCache& operator=(const FSourceImageThumbnailCache&) = delete;

		auto BeginFrame() -> void;
		auto Request(const FSourceImageThumbnailRequest& Request) -> void;
		auto Find(std::string_view Identity) const -> FAssetThumbnailView;
		auto EndFrame() -> void;
		auto CancelPendingRequests() -> void;
		auto Clear() -> void;
		auto Shutdown() -> void;
		auto IsShuttingDown() const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
