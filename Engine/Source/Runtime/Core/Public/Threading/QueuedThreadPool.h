#pragma once

#include "CoreAPI.h"

#include "HAL/Platform.h"
#include "Templates/MoveOnlyFunction.h"

namespace Durin
{
	using FQueuedWorkFunction = Private::TMoveOnlyFunction<void()>;
	using FQueuedWorkDiscardFunction = std::function<void()>;

	// Owns a fixed worker set and drains named work items from a shared queue.
	class FQueuedThreadPool
	{
	public:
		CORE_API FQueuedThreadPool();
		CORE_API ~FQueuedThreadPool();

		FQueuedThreadPool(const FQueuedThreadPool&) = delete;
		FQueuedThreadPool& operator=(const FQueuedThreadPool&) = delete;

		// WorkerCreationFailureIndex is an instrumented failure point used to validate partial-startup cleanup.
		CORE_API auto Create(uint32 InNumThreads, const char* InPoolName = "WorkerPool", uint32 WorkerCreationFailureIndex = std::numeric_limits<uint32>::max()) -> bool;
		CORE_API auto StopAcceptingWork() -> void;
		CORE_API auto Destroy(bool bWaitForQueuedWork = true) -> void;

		// Discard is invoked exactly once when accepted work is removed without execution.
		CORE_API auto Enqueue(const char* TaskName, FQueuedWorkFunction&& Work, FQueuedWorkDiscardFunction&& Discard = {}) -> bool;
		CORE_API auto TryExecuteOneQueuedTask() -> bool;

		// Returns false instead of deadlocking when called by one of this pool's workers.
		CORE_API auto WaitForIdle() -> bool;

		CORE_API auto GetNumThreads() const -> uint32;
		CORE_API auto GetNumQueuedTasks() const -> uint32;
		CORE_API auto IsRunning() const -> bool;

	private:
		class FImpl;

		std::unique_ptr<FImpl> Impl;
	};

	CORE_API auto GetDefaultThreadPoolThreadCount() -> uint32;
}
