#pragma once

#include "Misc/CoreStd.h"
#include "Misc/CoreTypes.h"

#include "Templates/MoveOnlyFunction.h"

#include "CoreAPI.h"

#include "HAL/Platform.h"

namespace Durin
{
	enum class EQueuedWorkPriority : uint8 { High, Normal, Low };

	using FQueuedWorkFunction = TMoveOnlyFunction<void()>;
	using FQueuedWorkDiscardFunction = TMoveOnlyFunction<void()>;

	namespace Private { struct FQueuedWorkNode; }

	// Weak observation of one stable queue node; never retains its callable.
	class FQueuedWorkHandle
	{
	private:
		friend class FQueuedThreadPool;
		std::weak_ptr<Private::FQueuedWorkNode> Node;
	};

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
		CORE_API auto Enqueue(
			const char* TaskName,
			FQueuedWorkFunction&& Work,
			FQueuedWorkDiscardFunction&& Discard = {},
			EQueuedWorkPriority Priority = EQueuedWorkPriority::Normal,
			FQueuedWorkHandle* OutHandle = nullptr
		) -> bool;
		// Claims the node in constant time under the queue lock. Stale/wrong-pool handles fail.
		CORE_API auto TryExecuteQueuedTask(const FQueuedWorkHandle& Handle) -> bool;
		// Internal scheduler path: the task itself owns its queue links and execution.
		CORE_API auto EnqueueNode(const std::shared_ptr<Private::FQueuedWorkNode>& Node, EQueuedWorkPriority Priority) -> bool;
		CORE_API auto TryExecuteNode(const std::shared_ptr<Private::FQueuedWorkNode>& Node) -> bool;

		// Returns false instead of deadlocking when called by one of this pool's workers.
		CORE_API auto WaitForIdle() -> bool;

		CORE_API auto GetNumThreads() const -> uint32;
		CORE_API auto GetNumQueuedTasks() const -> uint32;
		CORE_API auto IsRunning() const -> bool;

	private:
		class FImpl;

		std::unique_ptr<FImpl> Impl;
	};

	namespace Private { CORE_API auto SetQueuedWorkAllocationFailureForTests(bool bFail) -> void; }

	CORE_API auto GetDefaultThreadPoolThreadCount() -> uint32;
}
