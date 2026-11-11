#pragma once
#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"
#include "DObject/ObjectMacros.h"

namespace Durin::Private
{
	// The callback can inspect Reachable and restore editing flags before purge callbacks run.
	COREDOBJECT_API auto CollectGarbageWithPostReachability(EObjectFlags KeepFlags, bool bPerformFullPurge,
		const std::function<void()>& PostReachability) -> void;

	// Request intent only; Engine chooses retention policy at its safe point.
	COREDOBJECT_API auto IsFullPurgeRequested() -> bool;
}
