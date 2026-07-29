#pragma once

#include "CoreAPI.h"

#include "HAL/Platform.h"

namespace Durin
{
	using FQueuedWorkFunction = std::function<void()>;

	// Owns a fixed worker set and drains named work items from a shared queue.
	class FQueuedThreadPool
	{
	public:
		CORE_API FQueuedThreadPool();
		CORE_API ~FQueuedThreadPool();

		FQueuedThreadPool(const FQueuedThreadPool&) = delete;
		FQueuedThreadPool& operator=(const FQueuedThreadPool&) = delete;

		CORE_API auto Create(uint32 InNumThreads, const char* InPoolName = "WorkerPool") -> bool;
		CORE_API auto StopAcceptingWork() -> void;
		CORE_API auto Destroy(bool bWaitForQueuedWork = true) -> void;

		CORE_API auto Enqueue(const char* TaskName, FQueuedWorkFunction&& Work) -> bool;
		CORE_API auto TryExecuteOneQueuedTask() -> bool;

		CORE_API auto WaitForIdle() -> void;

		CORE_API auto GetNumThreads() const -> uint32;
		CORE_API auto GetNumQueuedTasks() const -> uint32;
		CORE_API auto IsRunning() const -> bool;

	private:
		class FImpl;

		std::unique_ptr<FImpl> Impl;
	};

	extern CORE_API FQueuedThreadPool* GThreadPool;

	CORE_API auto GetDefaultThreadPoolThreadCount() -> uint32;
	CORE_API auto InitEngineThreadPool(uint32 InNumThreads = 0) -> bool;
	CORE_API auto QuiesceEngineThreadPool() -> void;
	CORE_API auto ShutdownEngineThreadPool(bool bWaitForQueuedWork = true) -> void;
}
