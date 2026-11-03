#pragma once
#include "CoreMinimal.h"
#include "CacheBackend.h"
#include "BuildSchedulerPrivate.h"

namespace Durin::DerivedData::Private
{
	// Concrete cache composition. Request consumers depend only on ICache.
	DERIVEDDATACACHE_API auto CreateCacheForBackend(std::shared_ptr<ICacheBackend> Backend = {},
		std::shared_ptr<IBuildScheduler> Scheduler = {}, FCacheDispatchLimits Limits = {},
		FCacheResourceLimits ResourceLimits = {}) -> std::shared_ptr<ICache>;
}
