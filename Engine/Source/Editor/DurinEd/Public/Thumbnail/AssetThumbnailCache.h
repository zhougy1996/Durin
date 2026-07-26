#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	// Configures the provider-neutral persistent thumbnail object store.
	struct FAssetThumbnailObjectStoreSettings
	{
		std::filesystem::path CacheRoot;
		uint32 FormatVersion = 1;
		uint64 DiskBudgetBytes = 256ull * 1024ull * 1024ull;
		uint64 MaximumObjectBytes = 16ull * 1024ull * 1024ull;
		std::string ObjectExtension = ".png";
	};

	// Reports persistent object reuse and invalid-object cleanup.
	struct FAssetThumbnailObjectStoreStats
	{
		uint64 CacheHits = 0;
		uint64 Regenerations = 0;
	};

	enum class EAssetThumbnailObjectLoadResult : uint8
	{
		Miss,
		Hit,
		Invalid
	};

	// Owns a versioned, bounded object store keyed by provider-neutral thumbnail cache keys.
	class DURINED_API FAssetThumbnailObjectStore
	{
	public:
		explicit FAssetThumbnailObjectStore(FAssetThumbnailObjectStoreSettings Settings = {});
		~FAssetThumbnailObjectStore();

		FAssetThumbnailObjectStore(const FAssetThumbnailObjectStore&) = delete;
		FAssetThumbnailObjectStore& operator=(const FAssetThumbnailObjectStore&) = delete;

		auto Load(std::string_view Key, std::vector<uint8>& OutBytes) -> EAssetThumbnailObjectLoadResult;
		auto Store(std::string_view Key, std::span<const uint8> Bytes) -> bool;
		auto Invalidate(std::string_view Key) -> void;
		auto GetStats() const -> FAssetThumbnailObjectStoreStats;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	// Describes one CPU or GPU thumbnail allocation for a budget pass.
	struct FAssetThumbnailBudgetEntry
	{
		std::string Key;
		uint64 Bytes = 0;
		uint64 LastUsed = 0;
		bool bPinned = false;
	};

	// Selects least-recently-used, unpinned allocations until the requested budget is met.
	DURINED_API auto SelectAssetThumbnailBudgetEvictions(
		std::span<const FAssetThumbnailBudgetEntry> Entries, uint64 BudgetBytes) -> std::vector<std::string>;
} // namespace Durin
