#pragma once

#include "DurinEdAPI.h"
#include "Thumbnail/AssetThumbnailPool.h"

namespace Durin::Editor
{
	// Reports persistent object reuse and invalid-object cleanup.
	struct FThumbnailObjectStoreStats
	{
		uint64 CacheHits = 0;
		uint64 Regenerations = 0;
		uint64 Evictions = 0;
	};

	enum class EThumbnailObjectLoadResult : uint8
	{
		Miss,
		Hit,
		Invalid
	};

	// Owns a versioned, bounded object store keyed by provider-neutral thumbnail cache keys.
	class DURINED_API FThumbnailObjectStore
	{
	public:
		explicit FThumbnailObjectStore(FAssetThumbnailPoolStorageSettings Settings = {});
		~FThumbnailObjectStore();

		FThumbnailObjectStore(const FThumbnailObjectStore&) = delete;
		FThumbnailObjectStore& operator=(const FThumbnailObjectStore&) = delete;

		auto Load(std::string_view Key, std::vector<std::byte>& OutBytes) -> EThumbnailObjectLoadResult;
		auto Store(std::string_view Key, std::span<const std::byte> Bytes) -> bool;
		auto Invalidate(std::string_view Key) -> void;
		auto GetStats() const -> FThumbnailObjectStoreStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	// Describes one CPU or GPU thumbnail allocation for a budget pass.
	struct FThumbnailBudgetEntry
	{
		std::string Key;
		uint64 Bytes = 0;
		uint64 LastUsed = 0;
		bool bPinned = false;
	};

	// Selects least-recently-used, unpinned allocations until the requested budget is met.
	DURINED_API auto SelectThumbnailBudgetEvictions(
		std::span<const FThumbnailBudgetEntry> Entries, uint64 BudgetBytes) -> std::vector<std::string>;
} // namespace Durin::Editor
