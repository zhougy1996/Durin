#include "Threading/Task.h"

namespace Durin::Tasks
{
	namespace Detail
	{
		[[noreturn]] auto FailConstruction(FTaskConstructionError Error) -> void
		{
			requiref(false, "Task construction failed (code {}, task {}).", static_cast<uint32>(Error.Code), Error.RelatedTaskId);
			std::terminate();
		}

		auto WaitForSuccess(const Tasks::FTask& Handle) -> void
		{
			const auto WaitResult = Handle.WaitDetailed();
			require(WaitResult.WaitStatus == ETaskWaitStatus::Completed && WaitResult.TaskState == ETaskState::Succeeded);
		}
	}

	auto AddNested(const Tasks::FTask& Completion) -> void
	{
		if (auto Error = Durin::Private::FTaskRuntimeAccess::AddNested(Completion))
			Detail::FailConstruction(*Error);
	}

	FTaskEvent::FTaskEvent(const char* DebugName, bool bUnknownExecutionRequirement)
	{
		SetHandle(Durin::Private::FTaskRuntimeAccess::CreateTaskEvent(DebugName, bUnknownExecutionRequirement));
	}

	auto FTaskEvent::AddPrerequisites(std::span<const Durin::Tasks::FTask> Prerequisites) -> void
	{
		if (auto Error = Durin::Private::FTaskRuntimeAccess::AddTaskEventPrerequisites(*this, Prerequisites))
			Detail::FailConstruction(*Error);
	}

	auto FTaskEvent::Trigger() -> void
	{
		Durin::Private::FTaskRuntimeAccess::TriggerTaskEvent(*this);
	}

}
