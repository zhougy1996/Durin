#pragma once

#include "Thumbnail/AssetThumbnail.h"

namespace Durin
{
	// Coordinates asynchronous decode, GPU upload, memory eviction, and disk reuse.
	class FSourceImageThumbnailCache
	{
	public:
		FSourceImageThumbnailCache();
		~FSourceImageThumbnailCache();

		FSourceImageThumbnailCache(const FSourceImageThumbnailCache&) = delete;
		FSourceImageThumbnailCache& operator=(const FSourceImageThumbnailCache&) = delete;

		auto BeginFrame() -> void;
		auto Request(std::string_view PhysicalPath, uintmax_t FileSize, const std::filesystem::file_time_type& LastWriteTime, bool bVisible) -> void;
		auto Find(std::string_view PhysicalPath) const -> FAssetThumbnailView;
		auto EndFrame() -> void;
		auto CancelPendingRequests() -> void;
		auto Clear() -> void;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
