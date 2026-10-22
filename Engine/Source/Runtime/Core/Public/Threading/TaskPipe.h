#pragma once

#include "Threading/TaskComposition.h"

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
		// Blocks without retraction; own-pipe, independent-CPU and render contexts
		// cannot drain outstanding work. Use task Wait for dependency helping.
		auto WaitUntilEmpty(double TimeoutSeconds = std::numeric_limits<double>::infinity()) -> bool;

		template<typename F>
		auto Launch(const char* Name, F&& Function, FTaskExecutionOptions Options = {})
		{
			Options.DebugName = Name;
			return Detail::LaunchTaskImpl(ETaskExecutor::Worker, Options, std::forward<F>(Function), false, this);
		}
		template<typename F>
		auto Launch(const char* Name, F&& Function, ETaskPriority Priority)
		{ return Launch(Name, std::forward<F>(Function), FTaskExecutionOptions{.Priority = Priority}); }
		template<typename F, Detail::CTaskDependencyCollection R>
		auto Launch(const char* Name, F&& Function, const R& Inputs, FTaskExecutionOptions Options = {})
		{
			return Detail::WithPrerequisites(Options, Inputs, [&](const auto& BoundOptions) {
				return Launch(Name, std::forward<F>(Function), BoundOptions);
			});
		}
		template<typename F, Detail::CTaskDependency T>
		auto Launch(const char* Name, F&& Function, const T& Input, FTaskExecutionOptions Options = {})
		{ return Launch(Name, std::forward<F>(Function), Prerequisites(Input), Options); }
		template<typename F, typename R>
		requires (Detail::CTaskDependency<R> || Detail::CTaskDependencyCollection<R>)
		auto Launch(const char* Name, F&& Function, const R& Inputs, ETaskPriority Priority)
		{ return Launch(Name, std::forward<F>(Function), Inputs, FTaskExecutionOptions{.Priority = Priority}); }

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
