#pragma once

#include "Thumbnail/AssetThumbnailObjectStore.h"
#include "Thumbnail/RenderedAssetThumbnailPreviewScene.h"
#include "Thumbnail/AssetThumbnailProvider.h"

namespace Durin::Editor
{
	// Reports deterministic generation activity without exposing queue mechanics.
	struct FRenderedAssetThumbnailGenerationStats
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

	// Provides stable lifecycle and budget observations without exposing preview or UI objects.
	struct FRenderedAssetThumbnailCacheStats
	{
		FRenderedAssetThumbnailGenerationStats Generation;
		uint64 PreviewSceneCreations = 0;
		uint64 PreviewSceneAssignments = 0;
		uint64 UploadsQueued = 0;
		uint64 UploadsCompleted = 0;
		uint64 UploadFailures = 0;
		uint64 GpuEvictions = 0;
		uint64 LiveGpuTextures = 0;
		uint64 ParkedResourceWaits = 0;
		uint64 PeakParkedResourceWaits = 0;
		uint64 ResourceWaitTimeouts = 0;
		bool bHasActiveJob = false;
		bool bHasPreviewScene = false;
	};

	// Owns provider-neutral rendered generation, persistence, capture, upload, and UI texture lifetime.
	class FRenderedAssetThumbnailCache
	{
	public:
		DURINED_API explicit FRenderedAssetThumbnailCache(
			FAssetThumbnailBudgets Budgets = {},
			FAssetThumbnailObjectStoreSettings StoreSettings = {});
		DURINED_API explicit FRenderedAssetThumbnailCache(
			FAssetThumbnailProviderRegistry& Service,
			FAssetThumbnailBudgets Budgets = {},
			FAssetThumbnailObjectStoreSettings StoreSettings = {});
		DURINED_API ~FRenderedAssetThumbnailCache();

		FRenderedAssetThumbnailCache(const FRenderedAssetThumbnailCache&) = delete;
		FRenderedAssetThumbnailCache& operator=(const FRenderedAssetThumbnailCache&) = delete;

		DURINED_API auto BeginFrame() -> void;
		DURINED_API auto Request(
			const FAssetThumbnailPackageFingerprint& Asset,
			EAssetThumbnailPriority Priority) -> void;
		DURINED_API auto Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView;
		DURINED_API auto EndFrame() -> void;
		DURINED_API auto CancelPendingRequests() -> void;
		DURINED_API auto Clear() -> void;
		DURINED_API auto GetStats() const -> FRenderedAssetThumbnailCacheStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin::Editor
