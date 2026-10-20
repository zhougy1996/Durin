#pragma once

#include "CoreMinimal.h"

#include "Threading/TaskComposition.h"

namespace Durin
{
	namespace
	{
		// Kernel fixtures deliberately observe recoverable admission and erased lifetime state.
		// Ordinary result/acceptance behavior is exercised by TaskCompositionTests.
		template<typename F>
		auto SubmitCancelableKernelTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> Tasks::FTask
		{
			auto Admission = Private::TryLaunchCancelableTaskWithCompletion(Name, std::forward<F>(Function), {}, Options);
			return Admission.HasValue() ? std::move(Admission).TakeValue() : Tasks::FTask{};
		}
		template<typename F>
		auto SubmitKernelTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> Tasks::FTask
		{
			if constexpr (std::same_as<std::decay_t<F>, std::function<void()>>)
				if (!Function) return SubmitCancelableKernelTask(Name, Private::FMoveOnlyTaskFunction{}, Options);
			return SubmitCancelableKernelTask(Name,
				[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable { std::invoke(Function); }, Options);
		}
		template<typename F>
		auto SubmitKernelContinuation(const Tasks::FTask& Input, const char* Name, F&& Function,
			const FTaskContinuationOptions& Options = {}) -> Tasks::FTask
		{
			auto Admission = Private::TryLaunchContinuationTask(Input, Name,
				[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable { std::invoke(Function); },
				{}, Options, ETaskDependencyKind::Success);
			return Admission.HasValue() ? std::move(Admission).TakeValue() : Tasks::FTask{};
		}

	}
}
