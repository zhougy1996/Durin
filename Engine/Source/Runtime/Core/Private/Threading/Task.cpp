#include "Threading/Task.h"

#include "Threading/QueuedThreadPool.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		std::atomic<uint64> GNextTaskId = 1;
		constexpr double WorkerWaitSliceSeconds = 0.001;
	}

	class FTaskCompletionState
	{
	public:
		explicit FTaskCompletionState(const char* InDebugName)
			: TaskId(GNextTaskId.fetch_add(1, std::memory_order::acq_rel))
			, DebugName(InDebugName ? InDebugName : "Task")
		{
		}

		auto MarkComplete() -> void
		{
			{
				std::lock_guard Lock(Mutex);
				bCompleted = true;
			}
			CV.notify_all();
		}

		auto Wait() -> void
		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [this]() {
				return bCompleted;
			});
		}

		auto WaitFor(double TimeoutSeconds) -> bool
		{
			std::unique_lock Lock(Mutex);
			return CV.wait_for(Lock, std::chrono::duration<double>(TimeoutSeconds), [this]() {
				return bCompleted;
			});
		}

		auto IsComplete() const -> bool
		{
			std::lock_guard Lock(Mutex);
			return bCompleted;
		}

		auto GetDebugName() const -> const char*
		{
			return DebugName.c_str();
		}

		auto GetTaskId() const -> uint64
		{
			return TaskId;
		}

	private:
		uint64 TaskId = 0;
		std::string DebugName;
		mutable std::mutex Mutex;
		std::condition_variable CV;
		bool bCompleted = false;
	};

	FTaskHandle::FTaskHandle() = default;

	FTaskHandle::FTaskHandle(std::shared_ptr<FTaskCompletionState> InState)
		: State(std::move(InState))
	{
	}

	auto FTaskHandle::IsValid() const -> bool
	{
		return State != nullptr;
	}

	auto FTaskHandle::IsComplete() const -> bool
	{
		return State && State->IsComplete();
	}

	auto FTaskHandle::GetDebugName() const -> const char*
	{
		return State ? State->GetDebugName() : "";
	}

	auto LaunchTask(const char* Name, FTaskFunction&& Function) -> FTaskHandle
	{
		if (!Function)
		{
			DURIN_WARN("Task launch failed because the task function is empty. (task: {})", Name ? Name : "");
			return {};
		}

		if (!GThreadPool || !GThreadPool->IsRunning())
		{
			DURIN_WARN("Task launch failed because the engine thread pool is not running. (task: {})", Name ? Name : "");
			return {};
		}

		std::shared_ptr<FTaskCompletionState> CompletionState = std::make_shared<FTaskCompletionState>(Name);
		const char* TaskName = CompletionState->GetDebugName();
		if (!GThreadPool->Enqueue(TaskName, [CompletionState, Function = std::move(Function)]() mutable {
				Function();
				CompletionState->MarkComplete();
			}))
		{
			DURIN_WARN("Task launch failed because enqueue was rejected. (task: {}, id: {})", CompletionState->GetDebugName(), CompletionState->GetTaskId());
			return {};
		}

		DURIN_TRACE("Task launched. (task: {}, id: {})", CompletionState->GetDebugName(), CompletionState->GetTaskId());
		return FTaskHandle(std::move(CompletionState));
	}

	auto WaitTask(const FTaskHandle& Task) -> void
	{
		if (!Task.State)
		{
			return;
		}

		if (!IsInWorkerThread())
		{
			Task.State->Wait();
			return;
		}

		while (!Task.State->IsComplete())
		{
			if (GThreadPool && GThreadPool->TryExecuteOneQueuedTask())
			{
				continue;
			}

			Task.State->WaitFor(WorkerWaitSliceSeconds);
		}
	}

	auto WaitAll(std::span<const FTaskHandle> Tasks) -> void
	{
		for (const FTaskHandle& Task : Tasks)
		{
			WaitTask(Task);
		}
	}
}
