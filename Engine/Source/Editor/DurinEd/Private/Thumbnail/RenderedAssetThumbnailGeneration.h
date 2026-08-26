#pragma once

#include "Thumbnail/AssetThumbnailObjectStore.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "RenderedThumbnailRequestQueue.h"

namespace Durin::Editor
{
	// Identifies one cold generation while asynchronous game, render, and worker completions are outstanding.
	struct FRenderedAssetThumbnailJob
	{
		FRenderedThumbnailScheduledRequest ScheduledJob;
		uint64 AssetRevision = 0;
		uint64 ResourceRevision = 0;
	};

	// Distinguishes a persistent warm hit from cold provider work without loading the asset.
	struct FRenderedAssetThumbnailStartResult
	{
		std::optional<FRenderedAssetThumbnailJob> ColdJob;
		std::optional<FRenderedThumbnailScheduledRequest> WarmJob;
		std::vector<std::byte> EncodedBytes;
	};

	// Coordinates bounded rendered-thumbnail transitions and persistent publication across owning threads.
	class FRenderedAssetThumbnailGeneration
	{
	public:
		DURINED_API FRenderedAssetThumbnailGeneration(
			FRenderedThumbnailRequestQueue& Scheduler,
			FAssetThumbnailObjectStoreSettings StoreSettings = {},
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FRenderedAssetThumbnailGeneration();

		FRenderedAssetThumbnailGeneration(const FRenderedAssetThumbnailGeneration&) = delete;
		FRenderedAssetThumbnailGeneration& operator=(const FRenderedAssetThumbnailGeneration&) = delete;

		// Resets the render-start allowance; callers invoke this once on the game thread per editor frame.
		DURINED_API auto BeginFrame() -> void;
		// Returns cold work only; a persistent hit is published Ready without loading or rendering.
		DURINED_API auto StartNext() -> std::optional<FRenderedAssetThumbnailJob>;
		// Returns encoded warm-hit bytes to the UI upload path while preserving the cold-only convenience API.
		DURINED_API auto StartNextDetailed() -> FRenderedAssetThumbnailStartResult;
		// Starts only provider-generated pixels so they can bypass a resource-bound rendered job.
		DURINED_API auto StartNextGeneratedPixelsDetailed()
			-> FRenderedAssetThumbnailStartResult;
		DURINED_API auto CompleteLoad(
			FRenderedAssetThumbnailJob& Job, uint64 AssetRevision, std::string_view Error = {}) -> bool;
		// Leaves the job waiting when resources are not ready and consumes no render allowance.
		DURINED_API auto BeginRender(
			FRenderedAssetThumbnailJob& Job,
			bool bResourcesReady,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::string_view Error = {}) -> bool;
		DURINED_API auto CompleteRender(
			const FRenderedAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::string_view Error = {}) -> bool;
		DURINED_API auto CompleteReadback(
			const FRenderedAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::string_view Error = {}) -> bool;
		// Atomically stores encoded output before publishing Ready.
		DURINED_API auto CompleteEncoding(
			const FRenderedAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::span<const std::byte> EncodedBytes,
			std::string_view Error = {}) -> bool;
		// Encodes tightly packed RGBA8 pixels as the fixed PNG output before atomic publication.
		DURINED_API auto CompletePixels(
			const FRenderedAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::span<const std::byte> Pixels,
			uint32 Width,
			uint32 Height,
			std::string_view Error = {},
			std::function<std::string()> ValidateBeforePublication = {}) -> bool;
		// Publishes canonical provider-generated pixels without a preview scene or render allowance.
		DURINED_API auto CompleteGeneratedPixels(
			FRenderedAssetThumbnailJob& Job,
			uint64 AssetRevision,
			std::span<const std::byte> Pixels,
			uint32 Width,
			uint32 Height,
			std::string_view Error = {}) -> bool;
		DURINED_API auto Cancel(const FRenderedAssetThumbnailJob& Job) -> void;
		DURINED_API auto RecordRetry() -> void;
		DURINED_API auto InvalidatePersistentObject(std::string_view CacheKey) -> void;
		DURINED_API auto GetStats() const -> FRenderedAssetThumbnailGenerationStats;

	private:
		auto StartNextDetailed(bool bGeneratedPixelsOnly)
			-> FRenderedAssetThumbnailStartResult;

		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin::Editor
