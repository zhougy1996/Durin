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
		FCacheDispatchLimits CacheDispatchLimits;
		FCacheResourceLimits CacheLimits;
	};
} // namespace Durin::DerivedData::Private

namespace Durin::DerivedData::Private
{
	DERIVEDDATACACHE_API auto CreateBuildForBackend(FBuildBackendOptions Options = {}) -> std::shared_ptr<IBuild>;
}
