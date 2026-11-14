#pragma once

#include "CoreMinimal.h"
#include "Thumbnail/ThumbnailStorage.h"
#include "Thumbnail/AssetThumbnailTypes.h"
#include "Threading/Future.h"

namespace Durin
{
	struct FThumbnailCacheReadResult
	{
		FByteBuffer Pixels;
		bool bInvalid = false;
	};
	struct FThumbnailCacheStats
	{
		uint64 Evictions = 0, CacheWrites = 0, CacheWriteFailures = 0, CacheWritesSkipped = 0;
	};
	// Bounded asynchronous pixel storage. No scheduler, renderer or render-budget ownership.
	class FAssetThumbnailCache final
	{
	public:
		DURINED_API FAssetThumbnailCache(
			FThumbnailObjectStoreSettings Settings = {}, uint64 CpuBudgetBytes = 64ull * 1024ull * 1024ull);
		DURINED_API ~FAssetThumbnailCache();
		DURINED_API auto Load(std::string Key, FAssetThumbnailOutputSettings Output) -> TFuture<FThumbnailCacheReadResult>;
		DURINED_API auto Save(std::string Key, FByteView Pixels, uint32 Width, uint32 Height) -> void;
		DURINED_API auto Tick() -> void;
		DURINED_API auto WaitForTasksForTesting() -> void;
		DURINED_API auto GetRetainedBytes() const -> uint64;
		DURINED_API auto GetStats() const -> FThumbnailCacheStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
} // namespace Durin
