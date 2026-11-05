#pragma once

#include "CoreMinimal.h"
#include "DerivedDataCacheAPI.h"
#include "DerivedDataRequest.h"

namespace Durin::DerivedData
{
	class IBuildScheduler;
	DERIVEDDATACACHE_API auto CreateInlineBuildScheduler() -> std::shared_ptr<IBuildScheduler>;
	// Borrows the running Core task system; creates no threads or memory reservations.
	DERIVEDDATACACHE_API auto CreateTaskBuildScheduler() -> std::shared_ptr<IBuildScheduler>;
	DERIVEDDATACACHE_API auto CreateTaskCacheScheduler() -> std::shared_ptr<IBuildScheduler>;
}
