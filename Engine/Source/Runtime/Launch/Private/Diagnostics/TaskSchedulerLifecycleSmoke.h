#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	struct FTaskSchedulerLifecycleSmokeState;

	// Starts the opt-in process-shutdown workload while task admission remains open.
	auto BeginTaskSchedulerLifecycleSmoke()
		-> std::shared_ptr<FTaskSchedulerLifecycleSmokeState>;
	// Audits the retained workload after ShutdownTaskSystem closes and drains the scheduler.
	auto ValidateTaskSchedulerLifecycleSmoke(
		const std::shared_ptr<FTaskSchedulerLifecycleSmokeState>& State) -> void;
}
