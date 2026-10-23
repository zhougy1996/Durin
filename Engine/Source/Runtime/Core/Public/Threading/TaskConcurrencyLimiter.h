#pragma once

#include "Threading/TaskComposition.h"

namespace Durin::Tasks
{
	// Limits callable bodies, excluding capture destruction and nested completion. Work is
	// accepted by the scheduler immediately; no worker blocks on a permit.
	class CORE_API FTaskConcurrencyLimiter final
	{
	public:
		explicit FTaskConcurrencyLimiter(uint32 MaxConcurrency, ETaskPriority Priority = ETaskPriority::Normal);
		~FTaskConcurrencyLimiter();
		FTaskConcurrencyLimiter(const FTaskConcurrencyLimiter&) = delete;
		auto operator=(const FTaskConcurrencyLimiter&) -> FTaskConcurrencyLimiter& = delete;

		// Slot is unique among active tasks and lies in [0, MaxConcurrency).
		template<typename F>
		requires (std::is_invocable_v<F&, uint32> || std::is_invocable_v<F&, uint32, const FTaskCancellationToken&>)
		auto Push(const char* Name, F&& Function, FTaskCancellationToken Cancellation = {}) -> void
		try
		{
			PushImpl(Name, [Function = std::forward<F>(Function)](const FTaskCancellationToken& Token) mutable {
				const uint32 Slot = GetCurrentSlot();
				if constexpr (std::is_invocable_v<F&, uint32, const FTaskCancellationToken&>) std::invoke(Function, Slot, Token);
				else std::invoke(Function, Slot);
			}, std::move(Cancellation));
		}

		catch (const std::bad_alloc&) { Detail::FailConstruction({ETaskConstructionErrorCode::AllocationFailure}); }

		// Like UE 5.8's drain before its one-shot event wait, this waits for current
		// callable bodies even after an earlier successful wait. It does not join
		// capture destruction or nested tasks. Prefer a new limiter for another batch.
		// Stop producers before waiting. The object may be destroyed without waiting;
		// its accepted bodies retain the shared state through capture retirement.
		auto Wait(double TimeoutSeconds = std::numeric_limits<double>::infinity()) -> bool;

	private:
		static auto GetCurrentSlot() -> uint32;
		auto PushImpl(const char* Name, Durin::Private::FMoveOnlyTaskFunction&& Function,
			FTaskCancellationToken Cancellation) -> void;
		std::shared_ptr<Private::FTaskConcurrencyLimiterState> State;
	};
}
