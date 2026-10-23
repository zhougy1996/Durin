#pragma once

#include "Misc/CoreTypes.h"

#include "Misc/CoreStd.h"

#include "HAL/Platform.h"

namespace Durin::Tasks
{
	// Construction contract failures are separate from accepted task outcomes.
	enum class ETaskConstructionErrorCode : uint8
	{
		AllocationFailure,
		LifetimeClosed,
		InvalidPrerequisite,
		UnsupportedExecutor,
		DependencyCycle,
		InvalidCallable,
		InvalidExecutionContract,
	};

	// Zero means no particular task is associated with the rejection.
	struct FTaskConstructionError
	{
		ETaskConstructionErrorCode Code;
		uint64 RelatedTaskId = 0;
	};

}
