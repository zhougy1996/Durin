#pragma once

#include "Misc/CoreStd.h"
#include "CoreAPI.h"

namespace Durin::Private
{
	// Read-only identity. Owners can reject reuse across scheduler restarts
	// without registering a task scope or retaining the scheduler itself.
	CORE_API auto GetTaskSchedulerLifetime() -> std::weak_ptr<const void>;
}
