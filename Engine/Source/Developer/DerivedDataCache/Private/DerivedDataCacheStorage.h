#pragma once

#include "CoreMinimal.h"

#include "DerivedDataCache/DerivedDataCacheTypes.h"

namespace Durin::DerivedData
{
	enum class ECacheStorageNamespace : uint8 { Legacy, Records, Values };
	struct FCacheStorageGetRequest
	{
		FCacheKey Key;
		uint64 MaximumValueBytes = 0;
		ECacheStorageNamespace Namespace = ECacheStorageNamespace::Legacy;
		uint64* ReadBytes = nullptr;
		std::function<bool()> ShouldCancel;
	};

	using FCacheStorageGetResult =
		std::expected<std::optional<FSharedByteBuffer>, FCacheError>;
	// Shares only in-flight immutable reads; never retains a hot content cache.
	class FCacheReadCoalescer
	{
	public:
		DERIVEDDATACACHE_API auto Get(std::string Key, std::function<FCacheStorageGetResult()> Read, uint64* ReadBytes = nullptr, std::function<bool()> ShouldCancel = {})
			-> FCacheStorageGetResult;
		auto GetCoalescedReadCount() const -> uint64 { return CoalescedReads.load(); }
		auto GetPendingReadCount() -> size_t { std::lock_guard Lock(Mutex); return Pending.size(); }
	private:
		std::mutex Mutex;
		struct FRead { std::shared_future<FCacheStorageGetResult> Future; uint32 Waiters = 0; };
		// Overflow reads run independently; no unbounded retained key or waiter table.
		static constexpr uint32 MaximumReads = 64, MaximumWaiters = 64;
		std::unordered_map<std::string, std::shared_ptr<FRead>> Pending;
		std::atomic<uint64> CoalescedReads = 0;
	};

	struct FCacheStoragePutRequest
	{
		FCacheKey Key;
		FByteView Value;
		uint64 MaximumValueBytes = 0;
		ECacheStorageNamespace Namespace = ECacheStorageNamespace::Legacy;
	};

	using FCacheStoragePutResult = std::expected<void, FCacheError>;

	// Private bounded byte persistence; manifest and blob codecs own integrity.
	class FCacheStorage
	{
	public:
		DERIVEDDATACACHE_API auto Get(
			const FCacheStorageGetRequest& Request) const -> FCacheStorageGetResult;
		DERIVEDDATACACHE_API auto Put(
			const FCacheStoragePutRequest& Request) const -> FCacheStoragePutResult;
	};

	DERIVEDDATACACHE_API auto GetCacheStorage() -> FCacheStorage&;
}
