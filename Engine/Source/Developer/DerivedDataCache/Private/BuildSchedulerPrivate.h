#pragma once
#include "CoreMinimal.h"
#include "DerivedDataBuildScheduler.h"
#include "Threading/TaskRuntime.h"
namespace Durin::DerivedData
{
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
		virtual auto RequestCancel() const -> void {}
	};
	class IBuildScheduler
	{
	public:
		virtual ~IBuildScheduler() = default;
		virtual auto IsInline() const -> bool { return false; }
		// Reject without invoking/retaining Work, or accept at-most-once execution.
		// A null wait handle permits externally driven work; dropped work cancels.
		virtual auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> = 0;
	};
}
namespace Durin::DerivedData::Private
{
	// Native dispatch retirement is an implementation concern of owner waiting.
	class INativeBuildWork
	{
	public:
		virtual ~INativeBuildWork() = default;
		virtual auto GetTaskHandle() const -> Tasks::FTask = 0;
	};
	inline auto GetNativeTask(const IBuildScheduledWork& Work) -> Tasks::FTask
	{
		const auto* Native = dynamic_cast<const INativeBuildWork*>(&Work);
		return Native ? Native->GetTaskHandle() : Tasks::FTask{};
	}
} // namespace Durin::DerivedData::Private
