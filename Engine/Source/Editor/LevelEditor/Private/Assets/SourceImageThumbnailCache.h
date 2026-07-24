#pragma once

#include "RHIResources.h"

namespace Durin
{
	// Tracks whether a source thumbnail is absent, loading, ready, or failed.
	enum class ESourceImageThumbnailState : uint8
	{
		NotRequested,
		Queued,
		Decoding,
		Uploading,
		Ready,
		Failed
	};

	// Exposes one cache entry without transferring texture ownership.
	struct FSourceImageThumbnailView
	{
		ESourceImageThumbnailState State = ESourceImageThumbnailState::NotRequested;
		FRHITexture* Texture = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		bool bHasTransparency = false;
		std::string Error;
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
		auto Request(std::string_view PhysicalPath, uintmax_t FileSize, const std::filesystem::file_time_type& LastWriteTime, bool bVisible) -> void;
		auto Find(std::string_view PhysicalPath) const -> FSourceImageThumbnailView;
		auto EndFrame() -> void;
		auto CancelPendingRequests() -> void;
		auto Clear() -> void;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
