#pragma once
#include "CoreMinimal.h"
#include "DerivedDataBuildSession.h"
#include "DerivedDataCache/DerivedDataCache.h"
#include "CacheDispatchPrivate.h"
namespace Durin::DerivedData::Private
{
	struct FBuildBackendOptions
	{
		std::shared_ptr<ICacheBackend> Cache;
		std::shared_ptr<IAsyncCache> AsyncCache;
		FCacheDispatchLimits CacheDispatchLimits;
		FCacheResourceLimits CacheLimits;
	};
} // namespace Durin::DerivedData::Private

namespace Durin::DerivedData::Private
{
	inline auto CreateBuildForBackend(FBuildBackendOptions Options = {}) -> std::shared_ptr<IBuild>
	{
		return CreateBuild({.Cache = CreateCacheForBackend(std::move(Options.Cache), std::move(Options.AsyncCache), Options.CacheDispatchLimits, Options.CacheLimits), .CacheDispatchLimits = Options.CacheDispatchLimits, .CacheLimits = Options.CacheLimits});
	}
} // namespace Durin::DerivedData::Private
