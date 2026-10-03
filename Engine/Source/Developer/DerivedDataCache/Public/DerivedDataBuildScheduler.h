#pragma once

#include "CoreMinimal.h"
#include "DerivedDataBuildExecution.h"

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
	enum class EBuildWaitResult : uint8 { Completed, WouldBlock, InvalidRequest };
	enum class EBuildPriority : uint8 { High, Normal, Low };
	struct FBuildScheduleParams
	{
		std::string_view FunctionName;
		EBuildPriority Priority = EBuildPriority::Normal;
		// Already-admitted owner budget, not another reservation request.
		uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max();
	};
	class IBuildScheduledWork
	{
	public:
		virtual ~IBuildScheduledWork() = default;
		// May help execute tasks. Completed includes executing or dropping the thunk.
		virtual auto Wait() const -> EBuildWaitResult = 0;
	};
	class IBuildScheduler
	{
	public:
		virtual ~IBuildScheduler() = default;
		// Reject without invoking/retaining Work, or accept at-most-once execution.
		// A null wait handle permits externally driven work; dropped work cancels.
		virtual auto Schedule(const FBuildScheduleParams& Params, std::function<void()> Work)
			-> std::expected<std::shared_ptr<IBuildScheduledWork>, FBuildAdmissionError> = 0;
	};
	DERIVEDDATACACHE_API auto CreateInlineBuildScheduler() -> std::shared_ptr<IBuildScheduler>;
	// Borrows the running Core task system; creates no threads or memory reservations.
	DERIVEDDATACACHE_API auto CreateTaskBuildScheduler() -> std::shared_ptr<IBuildScheduler>;
}
