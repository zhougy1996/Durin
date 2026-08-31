#pragma once

#include "Thumbnail/ThumbnailManager.h"

namespace Durin::Editor
{
	struct FAssetThumbnailScheduledRequest
	{
		std::string CacheKey;
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
		FAssetThumbnailGenerationRequest GenerationRequest;
	};

	// Coalesces lightweight UI requests immediately, then captures renderer-owned
	// generation data lazily as scheduler work is admitted.
	class FAssetThumbnailRequestQueue
	{
	public:
		DURINED_API explicit FAssetThumbnailRequestQueue(
			DThumbnailManager& Registry,
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FAssetThumbnailRequestQueue();

		FAssetThumbnailRequestQueue(const FAssetThumbnailRequestQueue&) = delete;
		FAssetThumbnailRequestQueue& operator=(const FAssetThumbnailRequestQueue&) = delete;

		DURINED_API auto Request(const FAssetThumbnailRequest& Request, std::string& OutError) -> bool;
		DURINED_API auto Find(const FPackagePath& AssetPath) const -> FAssetThumbnailView;
		DURINED_API auto TakeNext() -> std::optional<FAssetThumbnailScheduledRequest>;
		// Selects renderer-generated pixels without waiting behind a resource-bound rendered job.
		DURINED_API auto TakeNextGeneratedPixels()
			-> std::optional<FAssetThumbnailScheduledRequest>;
		// Advances a captured job only while its key, renderer generation, serial,
		// identity, and revisions remain current.
		DURINED_API auto Transition(
			const FAssetThumbnailScheduledRequest& Job,
			EAssetThumbnailState ExpectedState,
			EAssetThumbnailState NextState,
			uint64 AssetRevision = 0,
			uint64 ResourceRevision = 0,
			std::string_view Diagnostic = {}) -> bool;
		DURINED_API auto Cancel(const FPackagePath& AssetPath) -> void;
		DURINED_API auto CancelAll() -> void;
		DURINED_API auto Shutdown() -> void;
		DURINED_API auto NumQueued() const -> size_t;
		DURINED_API auto IsShuttingDown() const -> bool;

	private:
		auto TakeNext(bool bGeneratedPixelsOnly)
			-> std::optional<FAssetThumbnailScheduledRequest>;

		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

} // namespace Durin::Editor
