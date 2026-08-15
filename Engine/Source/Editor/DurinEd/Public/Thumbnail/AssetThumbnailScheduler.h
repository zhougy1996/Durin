#pragma once

#include "Thumbnail/AssetThumbnailProvider.h"

namespace Durin::Editor
{
	struct FAssetThumbnailScheduledJob
	{
		std::string CacheKey;
		EAssetThumbnailPriority Priority = EAssetThumbnailPriority::Prefetch;
		FAssetThumbnailGenerationRequest GenerationRequest;
	};

	// Captures exact-class provider requests, coalesces cache keys, and enforces queue ordering and bounds.
	class FAssetThumbnailScheduler
	{
	public:
		DURINED_API explicit FAssetThumbnailScheduler(
			FAssetThumbnailProviderRegistry& Registry,
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FAssetThumbnailScheduler();

		FAssetThumbnailScheduler(const FAssetThumbnailScheduler&) = delete;
		FAssetThumbnailScheduler& operator=(const FAssetThumbnailScheduler&) = delete;

		DURINED_API auto Request(const FAssetThumbnailRequest& Request, std::string& OutError) -> bool;
		DURINED_API auto Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView;
		DURINED_API auto TakeNext() -> std::optional<FAssetThumbnailScheduledJob>;
		// Selects provider-generated pixels without waiting behind a resource-bound rendered job.
		DURINED_API auto TakeNextGeneratedPixels()
			-> std::optional<FAssetThumbnailScheduledJob>;
		// Advances a captured job only while its key, provider generation, serial, identity, and revisions remain current.
		DURINED_API auto Transition(
			const FAssetThumbnailScheduledJob& Job,
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
			-> std::optional<FAssetThumbnailScheduledJob>;

		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

} // namespace Durin::Editor
