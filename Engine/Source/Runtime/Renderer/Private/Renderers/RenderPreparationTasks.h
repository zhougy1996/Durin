#pragma once

#include "CoreMinimal.h"
#include "Threading/Task.h"

namespace Durin::RendererPrivate
{
	// The render thread may join independent CPU work. All ranges are submitted
	// before any join, and the caller owns the inputs and indexed output until it returns.
	template<typename F>
	auto RunPreparationRanges(const char* Name, size_t Count, size_t MinBatchSize, bool bAllowTasks,
		const F& Body) -> size_t
	{
		const auto RunRange = [&Body](size_t First, size_t Last) {
			for (size_t Index = First; Index < Last; ++Index) Body(Index);
		};
		if (Count != 0 && bAllowTasks && IsTaskSchedulerRunning())
		{
			constexpr size_t MaxTasks = 8;
			struct FPendingRanges
			{
				std::array<Tasks::FTask, MaxTasks> Handles;
				~FPendingRanges()
				{
					for (const auto& Task : Handles)
						if (Task.IsValid()) require(Task.Wait());
				}
			} Pending;
			const size_t BatchSize = std::max(MinBatchSize, 1 + (Count - 1) / MaxTasks);
			const size_t TaskCount = 1 + (Count - 1) / BatchSize;
			for (size_t Index = 0; Index < TaskCount; ++Index)
			{
				const size_t First = Index * BatchSize;
				const size_t Last = First + std::min(BatchSize, Count - First);
				Pending.Handles[Index] = Tasks::LaunchIndependentTask(Name, [RunRange, First, Last] {
					RunRange(First, Last);
				});
			}
			const auto Handles = std::span(Pending.Handles).first(TaskCount);
			require(Tasks::Wait(Handles));
			if (std::ranges::all_of(Handles, [](const auto& Task) {
				return Task.GetState() == ETaskState::Succeeded;
			}))
				return TaskCount;
		}
		RunRange(0, Count);
		return 0;
	}
}
