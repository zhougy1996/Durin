#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/ThumbnailStorage.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "AssetThumbnailRequestQueue.h"

namespace Durin
{
	// Distinguishes a persistent warm hit from cold renderer work without loading the asset.
	struct FAssetThumbnailStartResult
	{
		std::optional<FAssetThumbnailScheduledRequest> Job;
		FByteBuffer Pixels;
		bool bProgressed = false;
	};

	// Coordinates bounded rendered-thumbnail transitions and persistent publication across owning threads.
	class FAssetThumbnailGeneration
	{
	public:
		// Cache reads, encoding and persistence run on bounded background tasks.
		DURINED_API FAssetThumbnailGeneration(
			FAssetThumbnailRequestQueue& Scheduler,
			FThumbnailObjectStoreSettings StoreSettings = {},
			FAssetThumbnailBudgets Budgets = {});
		// Test-only drain; normal frames must never wait for cache work.
		DURINED_API auto WaitForCacheTasksForTesting() -> void;
		DURINED_API ~FAssetThumbnailGeneration();

		FAssetThumbnailGeneration(const FAssetThumbnailGeneration&) = delete;
		FAssetThumbnailGeneration& operator=(const FAssetThumbnailGeneration&) = delete;

		// Resets the render-start allowance; callers invoke this once on the game thread per editor frame.
		DURINED_API auto BeginFrame() -> void;
		// Returns completed warm pixels or cold work without waiting for cache I/O.
		DURINED_API auto PollNext() -> FAssetThumbnailStartResult;
		DURINED_API auto CompleteLoad(
			FAssetThumbnailScheduledRequest& Job, std::string_view Error = {}) -> bool;
		// Leaves the job waiting when resources are not ready and consumes no render allowance.
		DURINED_API auto BeginRender(
			FAssetThumbnailScheduledRequest& Job,
			bool bResourcesReady,
			std::string_view Error = {}) -> bool;
		// Accepts one completed capture, validates pixels and schedules persistence.
		DURINED_API auto CompleteCapture(
			const FAssetThumbnailScheduledRequest& Job,
			FByteView Pixels, uint32 Width, uint32 Height,
			std::string_view Error = {}) -> bool;
		DURINED_API auto Cancel(const FAssetThumbnailScheduledRequest& Job) -> void;
		DURINED_API auto RecordRetry() -> void;
		DURINED_API auto GetStats() const -> FAssetThumbnailGenerationStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
