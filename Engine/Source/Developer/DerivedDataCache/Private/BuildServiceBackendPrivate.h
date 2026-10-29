#pragma once
#include "CoreMinimal.h"
#include "DerivedDataBuildSession.h"
#include "CacheDispatchPrivate.h"
namespace Durin::DerivedData::Private
{
	struct FBuildBackendOptions
	{
		std::shared_ptr<ICacheBackend> Cache;
		std::shared_ptr<IAsyncCache> AsyncCache;
		FCacheDispatchLimits CacheDispatchLimits;
	};
} // namespace Durin::DerivedData::Private

namespace Durin::DerivedData::Private
{
	inline auto CreateBuildForBackend(FBuildBackendOptions Options = {}) -> std::shared_ptr<IBuild>
	{
		return CreateBuild({.Cache = CreateCacheForBackend(std::move(Options.Cache), std::move(Options.AsyncCache), Options.CacheDispatchLimits), .CacheDispatchLimits = Options.CacheDispatchLimits});
	}
} // namespace Durin::DerivedData::Private
