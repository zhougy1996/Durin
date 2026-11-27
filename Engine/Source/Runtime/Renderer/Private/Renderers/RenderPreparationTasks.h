#pragma once

#include "CoreMinimal.h"
#include "Threading/Task.h"

namespace Durin::RendererPrivate
{
	// The render thread may join independent CPU work. All ranges are submitted
	// before any join, and the caller owns the inputs and indexed output until it returns.
	// A failed batch is retried on the caller, so Body must support rewriting its
	// indexed output. Returns zero if any work required serial execution.
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
				bool bJoined = false;
				~FPendingRanges()
				{
					if (bJoined) return;
					for (const auto& Task : Handles)
						if (Task.IsValid()) require(Task.Wait());
				}
			} Pending;
			const size_t BatchSize = std::max(MinBatchSize, 1 + (Count - 1) / MaxTasks);
			const size_t TaskCount = 1 + (Count - 1) / BatchSize;
			const auto RunBatch = [RunRange, BatchSize, Count](size_t Index) {
				const size_t First = Index * BatchSize;
				RunRange(First, First + std::min(BatchSize, Count - First));
			};
			for (size_t Index = 0; Index < TaskCount; ++Index)
			{
				Pending.Handles[Index] = Tasks::LaunchTask(Name, [RunBatch, Index] { RunBatch(Index); });
			}
			const auto Handles = std::span(Pending.Handles).first(TaskCount);
			require(Tasks::Wait(Handles));
			Pending.bJoined = true;
			bool bRetried = false;
			for (size_t Index = 0; Index < TaskCount; ++Index)
			{
				if (Handles[Index].GetState() == ETaskState::Succeeded) continue;
				RunBatch(Index);
				bRetried = true;
			}
			return bRetried ? 0 : TaskCount;
		}
		RunRange(0, Count);
		return 0;
	}
}
