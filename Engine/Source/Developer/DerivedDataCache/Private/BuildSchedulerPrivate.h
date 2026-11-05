#pragma once
#include "CoreMinimal.h"
#include "DerivedDataBuildScheduler.h"
#include "Threading/TaskRuntime.h"
namespace Durin::DerivedData
{
	// Internal wait restrictions never form a public request result channel.
	enum class ERequestWaitResult : uint8 { Completed, WouldBlock };
	struct FBuildScheduleParams
	{
		std::string_view FunctionName;
		ERequestPriority Priority = ERequestPriority::Normal;
	};
	class IBuildScheduledWork
	{
	public:
		virtual ~IBuildScheduledWork() = default;
		// May help execute tasks. Completed includes executing or dropping the thunk.
		virtual auto Wait() const -> ERequestWaitResult = 0;
		virtual auto SetPriority(ERequestPriority) -> void {}
	};
	class IBuildScheduler
	{
	public:
		virtual ~IBuildScheduler() = default;
		virtual auto IsInline() const -> bool { return false; }
		// Executes or retains Work for at-most-once execution.
		// A null wait handle permits externally driven work; dropped work cancels.
		virtual auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Work)
			-> std::shared_ptr<IBuildScheduledWork> = 0;
	};
}
