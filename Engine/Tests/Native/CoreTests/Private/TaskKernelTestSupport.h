#pragma once

#include "CoreMinimal.h"

#include "Threading/Task.h"

namespace Durin
{
	namespace
	{
		// Kernel fixtures exercise erased callable and completion lifetime state.
		// Ordinary result/acceptance behavior is exercised by TaskCompositionTests.
		template<typename F>
		auto SubmitKernelTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> Tasks::FTask
		{
			return Private::LaunchTaskWithCompletion(Name, std::forward<F>(Function), {}, Options);
		}
	}
}
