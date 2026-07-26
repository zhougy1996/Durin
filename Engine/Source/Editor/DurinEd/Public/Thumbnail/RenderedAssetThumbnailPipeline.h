#pragma once

#include "Math/DurinMath.h"
#include "Thumbnail/AssetThumbnail.h"
#include "Thumbnail/AssetThumbnailCache.h"

namespace Durin
{
	class DStaticMesh;
	class PrimitiveSceneProxy;

	enum class ERenderedAssetThumbnailCaptureState : uint8
	{
		Idle,
		Rendering,
		Ready,
		Failed
	};

	// Owns the single resettable preview scene allowed by the initial rendered-thumbnail budget.
	class FRenderedAssetThumbnailPreviewScenePool
	{
	public:
		DURINED_API explicit FRenderedAssetThumbnailPreviewScenePool(
			FRenderedAssetThumbnailVisualContract VisualContract = {},
			FAssetThumbnailBudgets Budgets = {});
		DURINED_API ~FRenderedAssetThumbnailPreviewScenePool();

		FRenderedAssetThumbnailPreviewScenePool(const FRenderedAssetThumbnailPreviewScenePool&) = delete;
		FRenderedAssetThumbnailPreviewScenePool& operator=(const FRenderedAssetThumbnailPreviewScenePool&) = delete;

		DURINED_API auto IsAvailable() const -> bool;
		DURINED_API auto GetDiagnostic() const -> std::string;
		DURINED_API auto GetSphereMesh() const -> DStaticMesh*;
		// Replaces all provider-owned scene state while no capture is outstanding.
		DURINED_API auto SetPrimitive(
			std::unique_ptr<PrimitiveSceneProxy> Proxy,
			const FMatrix& Transform,
			std::string& OutError) -> bool;
		// Enqueues one render and one readback on the rendering thread.
		DURINED_API auto BeginCapture(std::string& OutError) -> bool;
		// Moves completed tightly-packed SRGBA8 pixels to the game thread.
		DURINED_API auto PollCapture(
			std::vector<uint8>& OutPixels,
			std::string& OutError) -> ERenderedAssetThumbnailCaptureState;
		DURINED_API auto Reset() -> void;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

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
			std::string_view Error = {}) -> bool;
		DURINED_API auto Cancel(const FRenderedAssetThumbnailJob& Job) -> void;
		DURINED_API auto RecordRetry() -> void;
		DURINED_API auto GetStats() const -> FRenderedAssetThumbnailPipelineStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
