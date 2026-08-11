#pragma once

#include "Thumbnail/AssetThumbnailObjectStore.h"
#include "Thumbnail/AssetThumbnailScheduler.h"

namespace Durin::Editor
{
	// Reports deterministic rendered-generation activity without exposing backend-specific objects.
	struct FRenderedAssetThumbnailPipelineStats
	{
		uint64 Jobs = 0;
		uint64 Loads = 0;
		uint64 ResourceWaits = 0;
		uint64 Renders = 0;
		uint64 Readbacks = 0;
		uint64 DiskHits = 0;
		uint64 Failures = 0;
		uint64 Retries = 0;
		uint64 Cancellations = 0;
		uint64 Evictions = 0;
	};

	// Identifies one cold generation while asynchronous game, render, and worker completions are outstanding.
	struct FRenderedAssetThumbnailJob
	{
		FAssetThumbnailScheduledJob ScheduledJob;
		uint64 AssetRevision = 0;
		uint64 ResourceRevision = 0;
	};

	// Distinguishes a persistent warm hit from cold provider work without loading the asset.
	struct FRenderedAssetThumbnailStartResult
	{
		std::optional<FRenderedAssetThumbnailJob> ColdJob;
		std::optional<FAssetThumbnailScheduledJob> WarmJob;
		std::vector<uint8> EncodedBytes;
	};

	// Coordinates bounded rendered-thumbnail transitions and persistent publication across owning threads.
	class FRenderedAssetThumbnailPipeline
	{
	public:
		DURINED_API FRenderedAssetThumbnailPipeline(
			FAssetThumbnailScheduler& Scheduler,
			FAssetThumbnailObjectStoreSettings StoreSettings = {},
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FRenderedAssetThumbnailPipeline();

		FRenderedAssetThumbnailPipeline(const FRenderedAssetThumbnailPipeline&) = delete;
		FRenderedAssetThumbnailPipeline& operator=(const FRenderedAssetThumbnailPipeline&) = delete;

		// Resets the render-start allowance; callers invoke this once on the game thread per editor frame.
		DURINED_API auto BeginFrame() -> void;
		// Returns cold work only; a persistent hit is published Ready without loading or rendering.
		DURINED_API auto StartNext() -> std::optional<FRenderedAssetThumbnailJob>;
		// Returns encoded warm-hit bytes to the UI upload path while preserving the cold-only convenience API.
		DURINED_API auto StartNextDetailed() -> FRenderedAssetThumbnailStartResult;
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
			std::span<const uint8> EncodedBytes,
			std::string_view Error = {}) -> bool;
		// Encodes tightly packed RGBA8 pixels as the fixed PNG output before atomic publication.
		DURINED_API auto CompletePixels(
			const FRenderedAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::span<const uint8> Pixels,
			uint32 Width,
			uint32 Height,
			std::string_view Error = {},
			std::function<std::string()> ValidateBeforePublication = {}) -> bool;
		DURINED_API auto Cancel(const FRenderedAssetThumbnailJob& Job) -> void;
		DURINED_API auto RecordRetry() -> void;
		DURINED_API auto InvalidatePersistentObject(std::string_view CacheKey) -> void;
		DURINED_API auto GetStats() const -> FRenderedAssetThumbnailPipelineStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin::Editor
