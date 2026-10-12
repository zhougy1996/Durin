#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildExecution.h"
#include "Threading/Task.h"
#include "DerivedDataRequest.h"

namespace Durin::DerivedData
{
	enum class EBuildAdmissionReason : uint8
	{
		Closed, Capacity, MissingFunction, InvalidRequest, DispatchRejected, InternalFailure
	};
	struct FBuildAdmissionError
	{
		static constexpr size_t MaximumDescriptionBytes = 4096;
		EBuildAdmissionReason Reason = EBuildAdmissionReason::InvalidRequest;
		std::string Description;
		auto BoundDescription() -> void
		{ if (Description.size() > MaximumDescriptionBytes) Description.resize(MaximumDescriptionBytes); }
	};
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
		// Native dispatch completion includes destruction of the scheduled thunk's captures.
		virtual auto GetTaskHandle() const -> FTaskHandle { return {}; }
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
	DERIVEDDATACACHE_API auto CreateInlineBuildScheduler() -> std::shared_ptr<IBuildScheduler>;
	// Borrows the running Core task system; creates no threads or memory reservations.
	DERIVEDDATACACHE_API auto CreateTaskBuildScheduler() -> std::shared_ptr<IBuildScheduler>;
	DERIVEDDATACACHE_API auto CreateTaskCacheScheduler() -> std::shared_ptr<IBuildScheduler>;
}
