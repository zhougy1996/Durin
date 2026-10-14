#pragma once

#include "Misc/CoreStd.h"

namespace Durin
{
	// Copies share a monotonic cancellation flag. Observers bridge existing cancellation
	// providers; their captures must remain valid for every retained copy, and must not
	// reenter this token. They are serialized and discarded with the last token copy.
	class FCancellationToken
	{
		struct FState { std::atomic<bool> Cancelled = false; };
		struct FObserver
		{
			std::mutex Mutex;
			std::function<bool()> Predicate;
			explicit FObserver(std::function<bool()> Value) : Predicate(std::move(Value)) {}
		};
	public:
		FCancellationToken() : State(std::make_shared<FState>()) {}
		template<typename F> requires std::is_invocable_r_v<bool, F&>
		FCancellationToken(F Predicate) : FCancellationToken()
		{ Observer = std::make_shared<FObserver>(std::move(Predicate)); }
		auto Cancel() const -> void { State->Cancelled.store(true, std::memory_order_release); }
		// Read the latched flag without invoking caller code (safe under lifecycle locks).
		auto IsCancellationRequested() const -> bool { return State->Cancelled.load(std::memory_order_acquire); }
		auto IsCancelled() const -> bool
		{
			if (IsCancellationRequested()) return true;
			if (Observer)
			{
				std::lock_guard Lock(Observer->Mutex);
				if (IsCancellationRequested()) return true;
				if (Observer->Predicate && Observer->Predicate()) Cancel();
			}
			return IsCancellationRequested();
		}
		// The added observer latches into the same flag, including for existing copies.
		auto WithObserver(std::function<bool()> Predicate) const -> FCancellationToken
		{
			auto Result = *this;
			Result.Observer = std::make_shared<FObserver>([Parent = *this, Predicate = std::move(Predicate)] {
				return Parent.IsCancelled() || (Predicate && Predicate());
			});
			return Result;
		}
	private:
		std::shared_ptr<FState> State;
		std::shared_ptr<FObserver> Observer;
	};
}
