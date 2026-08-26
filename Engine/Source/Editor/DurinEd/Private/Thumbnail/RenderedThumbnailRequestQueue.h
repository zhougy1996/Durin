#pragma once

#include "Thumbnail/AssetThumbnailProvider.h"

namespace Durin::Editor
{
	struct FRenderedThumbnailScheduledRequest
	{
		std::string CacheKey;
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
		FAssetThumbnailGenerationRequest GenerationRequest;
	};

	// Captures exact-class provider requests, coalesces cache keys, and enforces queue ordering and bounds.
	class FRenderedThumbnailRequestQueue
	{
	public:
		DURINED_API explicit FRenderedThumbnailRequestQueue(
			FAssetThumbnailProviderRegistry& Registry,
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FRenderedThumbnailRequestQueue();

		FRenderedThumbnailRequestQueue(const FRenderedThumbnailRequestQueue&) = delete;
		FRenderedThumbnailRequestQueue& operator=(const FRenderedThumbnailRequestQueue&) = delete;

		DURINED_API auto Request(const FAssetThumbnailRequest& Request, std::string& OutError) -> bool;
		DURINED_API auto Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView;
		DURINED_API auto TakeNext() -> std::optional<FRenderedThumbnailScheduledRequest>;
		// Selects provider-generated pixels without waiting behind a resource-bound rendered job.
		DURINED_API auto TakeNextGeneratedPixels()
			-> std::optional<FRenderedThumbnailScheduledRequest>;
		// Advances a captured job only while its key, provider generation, serial, identity, and revisions remain current.
		DURINED_API auto Transition(
			const FRenderedThumbnailScheduledRequest& Job,
			EAssetThumbnailState ExpectedState,
			EAssetThumbnailState NextState,
			uint64 AssetRevision = 0,
			uint64 ResourceRevision = 0,
			std::string_view Diagnostic = {}) -> bool;
		DURINED_API auto Cancel(const FAssetPath& AssetPath) -> void;
		DURINED_API auto CancelAll() -> void;
		DURINED_API auto Shutdown() -> void;
		DURINED_API auto NumQueued() const -> size_t;
		DURINED_API auto IsShuttingDown() const -> bool;

	private:
		auto TakeNext(bool bGeneratedPixelsOnly)
			-> std::optional<FRenderedThumbnailScheduledRequest>;

		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

} // namespace Durin::Editor
