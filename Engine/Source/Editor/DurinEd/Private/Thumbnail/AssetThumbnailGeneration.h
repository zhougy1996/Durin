#pragma once

#include "Thumbnail/ThumbnailStorage.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "AssetThumbnailRequestQueue.h"

namespace Durin::Editor
{
	// Identifies one cold generation while asynchronous game, render, and worker completions are outstanding.
	struct FAssetThumbnailJob
	{
		FAssetThumbnailScheduledRequest ScheduledJob;
		uint64 AssetRevision = 0;
		uint64 ResourceRevision = 0;
	};

	// Distinguishes a persistent warm hit from cold renderer work without loading the asset.
	struct FAssetThumbnailStartResult
	{
		std::optional<FAssetThumbnailJob> ColdJob;
		std::optional<FAssetThumbnailScheduledRequest> WarmJob;
		FByteArray EncodedBytes;
	};

	// Coordinates bounded rendered-thumbnail transitions and persistent publication across owning threads.
	class FAssetThumbnailGeneration
	{
	public:
		DURINED_API FAssetThumbnailGeneration(
			FAssetThumbnailRequestQueue& Scheduler,
			FAssetThumbnailPoolStorageSettings StoreSettings = {},
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FAssetThumbnailGeneration();

		FAssetThumbnailGeneration(const FAssetThumbnailGeneration&) = delete;
		FAssetThumbnailGeneration& operator=(const FAssetThumbnailGeneration&) = delete;

		// Resets the render-start allowance; callers invoke this once on the game thread per editor frame.
		DURINED_API auto BeginFrame() -> void;
		// Returns cold work only; a persistent hit is published Ready without loading or rendering.
		DURINED_API auto StartNext() -> std::optional<FAssetThumbnailJob>;
		// Returns encoded warm-hit bytes to the UI upload path while preserving the cold-only convenience API.
		DURINED_API auto StartNextDetailed() -> FAssetThumbnailStartResult;
		// Starts only renderer-generated pixels so they can bypass a resource-bound rendered job.
		DURINED_API auto StartNextGeneratedPixelsDetailed()
			-> FAssetThumbnailStartResult;
		DURINED_API auto CompleteLoad(
			FAssetThumbnailJob& Job, uint64 AssetRevision, std::string_view Error = {}) -> bool;
		// Leaves the job waiting when resources are not ready and consumes no render allowance.
		DURINED_API auto BeginRender(
			FAssetThumbnailJob& Job,
			bool bResourcesReady,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::string_view Error = {}) -> bool;
		DURINED_API auto CompleteRender(
			const FAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::string_view Error = {}) -> bool;
		DURINED_API auto CompleteReadback(
			const FAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::string_view Error = {}) -> bool;
		// Atomically stores encoded output before publishing Ready.
		DURINED_API auto CompleteEncoding(
			const FAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::span<const std::byte> EncodedBytes,
			std::string_view Error = {}) -> bool;
		// Encodes tightly packed RGBA8 pixels as the fixed PNG output before atomic publication.
		DURINED_API auto CompletePixels(
			const FAssetThumbnailJob& Job,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::span<const std::byte> Pixels,
			uint32 Width,
			uint32 Height,
			std::string_view Error = {},
			std::function<std::string()> ValidateBeforePublication = {}) -> bool;
		// Publishes canonical renderer-generated pixels without a preview scene or render allowance.
		DURINED_API auto CompleteGeneratedPixels(
			FAssetThumbnailJob& Job,
			uint64 AssetRevision,
			std::span<const std::byte> Pixels,
			uint32 Width,
			uint32 Height,
			std::string_view Error = {}) -> bool;
		DURINED_API auto Cancel(const FAssetThumbnailJob& Job) -> void;
		DURINED_API auto RecordRetry() -> void;
		DURINED_API auto InvalidatePersistentObject(std::string_view CacheKey) -> void;
		DURINED_API auto GetStats() const -> FAssetThumbnailGenerationStats;

	private:
		auto StartNextDetailed(bool bGeneratedPixelsOnly)
			-> FAssetThumbnailStartResult;

		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin::Editor
