#pragma once

#include "Thumbnail/ThumbnailPreviewScene.h"
#include "Thumbnail/ThumbnailManager.h"

namespace Durin::Editor
{
	// Configures the pool's compatible project-local persistent object domain.
	struct FAssetThumbnailPoolStorageSettings
	{
		std::filesystem::path CacheRoot;
		uint32 FormatVersion = 1;
		uint64 DiskBudgetBytes = 256ull * 1024ull * 1024ull;
		uint64 MaximumObjectBytes = 16ull * 1024ull * 1024ull;
		std::string ObjectExtension = ".png";
	};

	// Reports deterministic generation activity without exposing queue mechanics.
	struct FAssetThumbnailGenerationStats
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
	struct FAssetThumbnailPoolStats
	{
		FAssetThumbnailGenerationStats Generation;
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
		uint64 QueuedJobs = 0;
		uint64 RetainedEntries = 0;
		uint64 PinnedEntries = 0;
		uint64 Referencers = 0;
		bool bHasActiveJob = false;
		bool bHasPreviewScene = false;
	};

	// Owns renderer-neutral rendered generation, persistence, capture, upload, and UI texture lifetime.
	class FAssetThumbnailPool
	{
	public:
		DURINED_API explicit FAssetThumbnailPool(
			FAssetThumbnailBudgets Budgets = {},
			FAssetThumbnailPoolStorageSettings StoreSettings = {});
		DURINED_API explicit FAssetThumbnailPool(
			DThumbnailManager& Manager,
			FAssetThumbnailBudgets Budgets = {},
			FAssetThumbnailPoolStorageSettings StoreSettings = {});
		DURINED_API ~FAssetThumbnailPool();

		FAssetThumbnailPool(const FAssetThumbnailPool&) = delete;
		FAssetThumbnailPool& operator=(const FAssetThumbnailPool&) = delete;

		DURINED_API auto BeginFrame() -> void;
		DURINED_API auto Request(
			const FAssetThumbnailPackageFingerprint& Asset,
			EAssetThumbnailPriority Priority) -> void;
		DURINED_API auto Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView;
		DURINED_API auto AddReferencer(const FAssetPath& AssetPath) -> void;
		DURINED_API auto RemoveReferencer(const FAssetPath& AssetPath) -> void;
		DURINED_API auto Refresh(const FAssetPath& AssetPath) -> void;
		DURINED_API auto EndFrame() -> void;
		DURINED_API auto CancelPendingRequests() -> void;
		DURINED_API auto Clear() -> void;
		DURINED_API auto GetStats() const -> FAssetThumbnailPoolStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin::Editor
