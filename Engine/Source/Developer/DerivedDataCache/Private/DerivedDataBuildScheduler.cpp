#include "DerivedDataBuildScheduler.h"
#include "Threading/TaskComposition.h"

namespace Durin::DerivedData
{
	namespace
	{
		class FInlineScheduler final : public IBuildScheduler
		{
		public:
			auto Schedule(const FBuildScheduleParams&, std::function<void()> Work)
				-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
			{ Work(); return std::shared_ptr<IBuildScheduledWork>{}; }
		};
		class FTaskWork final : public IBuildScheduledWork
		{
		public:
			Tasks::FTask Task;
			auto Wait() const -> EBuildWaitResult override
			{
				return Task.Wait().WaitStatus == ETaskWaitStatus::Completed
					? EBuildWaitResult::Completed : EBuildWaitResult::WouldBlock;
			}
		};
		class FTaskScheduler final : public IBuildScheduler
		{
		public:
			auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Work)
				-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> override
			{
				if (!IsTaskSchedulerRunning())
					return std::unexpected(FBuildAdmissionError{EBuildAdmissionReason::DispatchRejected,
						"Core task scheduler is not running."});
				auto Scheduled = std::make_shared<FTaskWork>();
				Tasks::FTaskExecutionOptions Options;
				Options.DebugName = "DerivedDataBuild";
				Options.Priority = Params.Priority == EBuildPriority::High ? ETaskPriority::High
					: Params.Priority == EBuildPriority::Low ? ETaskPriority::Low : ETaskPriority::Normal;
				Scheduled->Task = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, Options,
					[Work = std::move(Work)] { Work(); });
				return Scheduled;
			}
		};
	}
	auto CreateInlineBuildScheduler() -> std::shared_ptr<IBuildScheduler>
	{ return std::make_shared<FInlineScheduler>(); }
	auto CreateTaskBuildScheduler() -> std::shared_ptr<IBuildScheduler>
	{ return std::make_shared<FTaskScheduler>(); }
}
