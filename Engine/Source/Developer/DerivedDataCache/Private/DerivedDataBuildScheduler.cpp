#include "BuildSchedulerPrivate.h"
#include "Threading/Task.h"

namespace Durin::DerivedData
{
	namespace
	{
		class FInlineScheduler final : public IBuildScheduler
		{
		public:
			auto IsInline() const -> bool override { return true; }
			auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
				-> std::shared_ptr<IBuildScheduledWork> override
			{ Work(); return std::shared_ptr<IBuildScheduledWork>{}; }
		};
		class FTaskWork final : public IBuildScheduledWork
		{
		public:
			Tasks::FTask Task;
			auto SetPriority(ERequestPriority Priority) -> void override
			{
				Durin::Private::FTaskRuntimeAccess::SetPriority(Task, Priority == ERequestPriority::High ? ETaskPriority::High : Priority == ERequestPriority::Low ? ETaskPriority::Low : ETaskPriority::Normal);
			}
			auto Wait() const -> ERequestWaitResult override
			{
				return WaitTask(Task).WaitStatus == ETaskWaitStatus::Completed
					? ERequestWaitResult::Completed : ERequestWaitResult::WouldBlock;
			}
		};
		class FTaskScheduler final : public IBuildScheduler
		{
		public:
			explicit FTaskScheduler(bool IO = false) : IO(IO) {}
			auto IsInline() const -> bool override { return !IsTaskSchedulerRunning(); }
			auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Work)
				-> std::shared_ptr<IBuildScheduledWork> override
			{
				if (!IsTaskSchedulerRunning()) { Work(); return std::shared_ptr<IBuildScheduledWork>{}; }
				auto Scheduled = std::make_shared<FTaskWork>();
				Tasks::FTaskExecutionOptions Options;
				const std::string Name(Params.FunctionName.empty() ? (IO ? "DerivedDataCacheIO" : "DerivedDataBuild") : Params.FunctionName);
				// The DDC owner owns work lifetime; queue pumps must not donate their Core scope.
				Options.DebugName = Name.c_str();
				Options.Priority = Params.Priority == ERequestPriority::High ? ETaskPriority::High
					: Params.Priority == ERequestPriority::Low ? ETaskPriority::Low : ETaskPriority::Normal;
				Scheduled->Task = Tasks::LaunchTask(IO ? Tasks::ETaskExecutor::BlockingIO : Tasks::ETaskExecutor::Worker,
					Options, std::move(Work));
				return Scheduled;
			}
		private:
			bool IO;
		};
	}
	auto CreateInlineBuildScheduler() -> std::shared_ptr<IBuildScheduler>
	{ return std::make_shared<FInlineScheduler>(); }
	auto CreateTaskBuildScheduler() -> std::shared_ptr<IBuildScheduler>
	{ return std::make_shared<FTaskScheduler>(); }
	auto CreateTaskCacheScheduler() -> std::shared_ptr<IBuildScheduler>
	{ return std::make_shared<FTaskScheduler>(true); }
}
