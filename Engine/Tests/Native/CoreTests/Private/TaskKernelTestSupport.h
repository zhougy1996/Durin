#pragma once

#include "Threading/TaskComposition.h"

namespace Durin
{
	namespace
	{
		// Kernel fixtures deliberately observe recoverable admission and erased lifetime state.
		// Ordinary result/acceptance behavior is exercised by TaskCompositionTests.
		template<typename F>
		auto SubmitCancelableKernelTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle
		{
			auto Admission = Private::TryLaunchCancelableTaskWithCompletion(Name, std::forward<F>(Function), {}, Options);
			return Admission.HasValue() ? std::move(Admission).TakeValue() : FTaskHandle{};
		}
		template<typename F>
		auto SubmitKernelTask(const char* Name, F&& Function, const FTaskLaunchOptions& Options = {}) -> FTaskHandle
		{
			if constexpr (std::same_as<std::decay_t<F>, FTaskFunction>)
				if (!Function) return SubmitCancelableKernelTask(Name, Private::FMoveOnlyTaskFunction{}, Options);
			return SubmitCancelableKernelTask(Name,
				[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable { std::invoke(Function); }, Options);
		}
		template<typename F>
		auto SubmitKernelContinuation(const FTaskHandle& Input, const char* Name, F&& Function,
			const FTaskContinuationOptions& Options = {}) -> FTaskHandle
		{
			auto Admission = Private::TryLaunchContinuationTask(Input, Name,
				[Function = std::forward<F>(Function)](const FTaskCancellationToken&) mutable { std::invoke(Function); },
				{}, Options, ETaskDependencyKind::Success);
			return Admission.HasValue() ? std::move(Admission).TakeValue() : FTaskHandle{};
		}

	}
}
