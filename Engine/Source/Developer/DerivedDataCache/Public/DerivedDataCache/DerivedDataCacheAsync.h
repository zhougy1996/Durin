#pragma once

#include "CoreMinimal.h"
#include "DerivedDataCache/DerivedDataCache.h"
#include "DerivedDataBuildScheduler.h"
#include "DerivedDataRequestOwner.h"

namespace Durin::DerivedData
{
	namespace Private { class FAsyncCache; }
	using FCacheGetCallback = std::function<void(uint32, FCacheGetResult, FCacheQueryStats)>;
	using FCachePutCallback = std::function<void(uint32, FCachePutResult)>;
	class IAsyncCache
	{
	public:
		virtual ~IAsyncCache() = default;
		// Atomic batch admission. Accepted items complete exactly once, including drops.
		// IDs and records are retained; Stats pointers are ignored (stats travel in callback).
		// Captured cancellation/callback state must remain valid until the owner drains.
		virtual auto Get(std::span<const FCacheGetRequest> Requests, IRequestOwner& Owner, FCacheGetCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError> = 0;
		virtual auto Put(std::span<const FCachePutRequest> Requests, IRequestOwner& Owner, FCachePutCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError> = 0;
		virtual auto Close() -> void = 0;
		virtual auto Drain() -> ERequestWaitResult = 0;
	};
	// Concurrency bounds the whole I/O + codec operation; capacity bounds accepted items.
	DERIVEDDATACACHE_API auto CreateAsyncCache(std::shared_ptr<ICache> Cache = {},
		std::shared_ptr<IBuildScheduler> Scheduler = {}, uint32 Concurrency = 2, uint32 Capacity = 4096)
		-> std::shared_ptr<IAsyncCache>;
}
