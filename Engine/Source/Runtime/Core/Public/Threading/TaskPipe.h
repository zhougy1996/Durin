#pragma once

#include "Threading/Task.h"

namespace Durin::Tasks
{
	// Prerequisites resolve before entering the pipe. Ready tasks execute FIFO,
	// each after its predecessor completes (including nested work).
	class CORE_API FPipe final
	{
	public:
		explicit FPipe(const char* InDebugName = "Pipe") : DebugName(InDebugName ? InDebugName : "Pipe") {}
		~FPipe();
		FPipe(const FPipe&) = delete;
		auto operator=(const FPipe&) -> FPipe& = delete;
		FPipe(FPipe&&) = delete;
		auto operator=(FPipe&&) -> FPipe& = delete;

		auto GetDebugName() const -> const char* { return DebugName; }
		auto HasWork() const -> bool;
		auto IsInContext() const -> bool;
		// Stop producers first and ensure prerequisites can progress independently.
		// Blocks without retraction; own-pipe and render contexts
		// cannot drain outstanding work. Use task Wait for dependency helping.
		auto WaitUntilEmpty(double TimeoutSeconds = std::numeric_limits<double>::infinity()) -> bool;

		template<typename F>
		auto Launch(const char* Name, F&& Function,
			ETaskPriority Priority = ETaskPriority::Normal, FTaskAttribution Attribution = {})
		{
			return Detail::LaunchTaskImpl(Name, std::forward<F>(Function),
				{.Pipe = this, .Attribution = Attribution, .Priority = Priority});
		}

		template<typename F>
		auto Launch(const char* Name, F&& Function, FTaskAttribution Attribution)
		{
			return Launch(Name, std::forward<F>(Function), ETaskPriority::Normal, Attribution);
		}

		template<typename F, Detail::CTaskPrerequisites R>
		auto Launch(const char* Name, F&& Function, const R& Inputs,
			ETaskPriority Priority = ETaskPriority::Normal, FTaskAttribution Attribution = {})
		{
			return Detail::LaunchAfter(Name, std::forward<F>(Function), Inputs,
				{.Pipe = this, .Attribution = Attribution, .Priority = Priority});
		}

		template<typename F, Detail::CTaskPrerequisites R>
		auto Launch(const char* Name, F&& Function, const R& Inputs, FTaskAttribution Attribution)
		{
			return Launch(Name, std::forward<F>(Function), Inputs, ETaskPriority::Normal, Attribution);
		}

	private:
		friend class Durin::FTaskStateData;
		auto AddTask() -> void;
		auto RemoveTask() -> void;
		const char* DebugName;
		mutable std::mutex Mutex;
		std::condition_variable Empty;
		uint64 TaskCount = 0;
		// Protected by the runtime dependency-graph mutation lock.
		std::weak_ptr<FTaskStateData> LastTask;
	};
}
