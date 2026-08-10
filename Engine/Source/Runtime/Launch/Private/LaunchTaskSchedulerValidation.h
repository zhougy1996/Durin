#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	struct FLaunchTaskSchedulerValidationState;

	// Starts the opt-in process-shutdown workload while task admission remains open.
	auto BeginLaunchTaskSchedulerValidation()
		-> std::shared_ptr<FLaunchTaskSchedulerValidationState>;
	// Audits the retained workload after ShutdownTaskSystem closes and drains the scheduler.
	auto ValidateLaunchTaskSchedulerShutdown(
		const std::shared_ptr<FLaunchTaskSchedulerValidationState>& State) -> void;
}
