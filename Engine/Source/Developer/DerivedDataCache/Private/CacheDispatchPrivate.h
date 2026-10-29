#pragma once

#include "CoreMinimal.h"
#include "CacheBackend.h"
#include "RequestOwnerInternal.h"
#include "DerivedDataBuildScheduler.h"
#include "DerivedDataRequestOwner.h"

namespace Durin::DerivedData
{
	namespace Private
	{
		class FAsyncCache;
	}
	using FCacheGetCallback = std::function<void(uint32, FCacheGetResult, FCacheQueryStats)>;
	using FCachePutCallback = std::function<void(uint32, FCachePutResult)>;
	class IAsyncCache
	{
	public:
		virtual ~IAsyncCache() = default;
		// Atomic batch admission. Accepted items complete exactly once, including drops.
		// IDs and records are retained; Stats pointers are ignored (stats travel in callback).
		// Captured cancellation/callback state must remain valid until the owner drains.
		virtual auto Get(std::span<const FCacheGetBackendRequest> Requests, Private::IRequestOwnerInternal& Owner, FCacheGetCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError> = 0;
		virtual auto Put(std::span<const FCachePutBackendRequest> Requests, Private::IRequestOwnerInternal& Owner, FCachePutCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError> = 0;
		auto Get(std::span<const FCacheGetBackendRequest> Requests, IRequestOwner& Owner, FCacheGetCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError>
		{
			return Get(Requests, *Private::FOwnerAccess::Get(Owner), std::move(Callback));
		}
		auto Put(std::span<const FCachePutBackendRequest> Requests, IRequestOwner& Owner, FCachePutCallback Callback)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FCacheError>
		{
			return Put(Requests, *Private::FOwnerAccess::Get(Owner), std::move(Callback));
		}
		virtual auto Close() -> void = 0;
		virtual auto Drain() -> ERequestWaitResult = 0;
	};
	// Limits bound I/O concurrency and retained accepted requests through retirement.
	// Callable captures and backend/codec workspace are excluded from retained bytes.
	DERIVEDDATACACHE_API auto CreateAsyncCache(std::shared_ptr<ICacheBackend> Cache = {}, std::shared_ptr<IBuildScheduler> Scheduler = {}, FCacheDispatchLimits Limits = {})
		-> std::shared_ptr<IAsyncCache>;
} // namespace Durin::DerivedData

namespace Durin::DerivedData::Private
{
	struct FCacheAccess : std::enable_shared_from_this<FCacheAccess>
	{
		std::mutex Mutex;
		std::shared_ptr<ICacheBackend> Backend;
		std::shared_ptr<IAsyncCache> Override, Inline, Task;
		FCacheDispatchLimits Limits;
		FCacheResourceLimits ResourceLimits;
		static auto Wrap(std::shared_ptr<FCacheAccess> State) -> std::shared_ptr<ICache>
		{
			return std::shared_ptr<ICache>(new ICache(std::move(State)));
		}
		static auto Get(ICache& Cache) -> FCacheAccess& { return *Cache.State; }
		auto Dispatcher(bool bTask) -> std::shared_ptr<IAsyncCache>;
	};
	DERIVEDDATACACHE_API auto CreateCacheForBackend(std::shared_ptr<ICacheBackend> Backend = {}, std::shared_ptr<IAsyncCache> Override = {}, FCacheDispatchLimits Limits = {}, FCacheResourceLimits ResourceLimits = {}) -> std::shared_ptr<ICache>;

} // namespace Durin::DerivedData::Private
