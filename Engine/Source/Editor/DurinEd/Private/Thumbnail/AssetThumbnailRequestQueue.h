#pragma once

#include "CoreMinimal.h"

#include "ThumbnailGenerationRequest.h"

namespace Durin
{
	class DThumbnailManager;

	enum class EThumbnailJobState : uint8
	{
		NotRequested,
		Queued,
		Loading,
		WaitingForResources,
		Rendering,
		PixelsReady,
		Invalid,
		Failed
	};

	// Admission outcomes are scheduling policy, not generation errors.
	enum class EThumbnailRequestStatus : uint8
	{
		Accepted,
		// Existing equal or newer work is retained; no new work is queued.
		Coalesced,
		QueueFull,
		Unsupported,
		ShuttingDown,
	};

	inline auto IsThumbnailRequestAccepted(EThumbnailRequestStatus Status) -> bool
	{
		return Status == EThumbnailRequestStatus::Accepted || Status == EThumbnailRequestStatus::Coalesced;
	}

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

		DURINED_API auto Request(const FAssetThumbnailRequest& Request) -> EThumbnailRequestStatus;
		DURINED_API auto BeginFrame() -> void;
		DURINED_API auto GetState(const FTopLevelAssetPath& AssetPath) const -> EThumbnailJobState;
		DURINED_API auto GetFailureReason(const FTopLevelAssetPath& AssetPath) const -> std::string_view;
		DURINED_API auto TakeNext() -> std::optional<FAssetThumbnailScheduledRequest>;
		// Advances a captured job only while its key, renderer generation, serial,
		// and asset identity remain current. Resource validity belongs to the draw job.
		DURINED_API auto Transition(
			const FAssetThumbnailScheduledRequest& Job,
			EThumbnailJobState ExpectedState,
			EThumbnailJobState NextState,
			std::string_view Diagnostic = {}) -> bool;
		DURINED_API auto Cancel(const FTopLevelAssetPath& AssetPath) -> void;
		DURINED_API auto CancelRenderer(uint64 Generation) -> void;
		DURINED_API auto CancelAll() -> void;
		DURINED_API auto Shutdown() -> void;
		DURINED_API auto NumQueued() const -> size_t;
		DURINED_API auto IsShuttingDown() const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

} // namespace Durin
