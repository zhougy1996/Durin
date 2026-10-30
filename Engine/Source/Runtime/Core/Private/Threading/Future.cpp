#include "Threading/Future.h"

namespace Durin
{
	FFutureReadiness::FFutureReadiness() : State(std::make_shared<std::atomic<bool>>(false)) {}

	namespace FuturePrivate
	{
		auto FState::GetState() const -> EFutureState { std::lock_guard Lock(Mutex); return State; }
		auto FState::Wait() const -> void
		{ std::unique_lock Lock(Mutex); Changed.wait(Lock, [&] { return State != EFutureState::Pending; }); }
		auto FState::WaitFor(std::chrono::steady_clock::duration Timeout) const -> bool
		{
			std::unique_lock Lock(Mutex);
			if (Timeout == std::chrono::steady_clock::duration::max())
			{ Changed.wait(Lock, [&] { return State != EFutureState::Pending; }); return true; }
			return Changed.wait_for(Lock, Timeout, [&] { return State != EFutureState::Pending; });
		}
		auto FState::GetFailure() const -> FFutureFailure
		{
			std::lock_guard Lock(Mutex);
			if (State != EFutureState::Failed) throw std::logic_error("Future has no failure.");
			return Failure;
		}
		auto FState::CheckSuccess() const -> void
		{
			Wait();
			if (GetState() == EFutureState::Succeeded) return;
			if (GetState() == EFutureState::Failed)
			{
				const auto Error = GetFailure();
				if (Error.Exception) std::rethrow_exception(Error.Exception);
				if (Error.Code == EFutureFailure::Abandoned) throw std::runtime_error("Promise abandoned.");
			}
			throw std::runtime_error("Future did not succeed.");
		}
		auto FState::Claim() -> bool
		{ std::lock_guard Lock(Mutex); return !std::exchange(Claimed, true); }
		auto FState::Publish(EFutureState Outcome, FFutureFailure Error) -> void
		{
			decltype(Observers) Pending;
			{
				std::lock_guard Lock(Mutex);
				Failure = std::move(Error);
				State = Outcome;
				Pending.swap(Observers);
			}
			Changed.notify_all();
			for (const auto& Weak : Pending)
				if (auto Callback = Weak.lock()) { try { (*Callback)(Outcome); } catch (...) {} }
		}
		auto FState::Subscribe(std::function<void(EFutureState)> Callback) -> FFutureSubscription
		{
			if (!Callback) throw std::invalid_argument("Empty future observer.");
			FFutureSubscription Subscription;
			Subscription.Callback = std::make_shared<std::function<void(EFutureState)>>(std::move(Callback));
			EFutureState Outcome;
			{
				std::lock_guard Lock(Mutex);
				Outcome = State;
				if (State == EFutureState::Pending)
				{
					std::erase_if(Observers, [](const auto& Weak) { return Weak.expired(); });
					Observers.push_back(Subscription.Callback);
				}
			}
			if (Outcome != EFutureState::Pending)
			{ try { (*Subscription.Callback)(Outcome); } catch (...) {} }
			return Subscription;
		}
		auto FState::ObserveReady(const FFutureReadiness& Readiness) -> FFutureSubscription
		{ return Subscribe([Marker = Readiness.State](EFutureState) { Marker->store(true, std::memory_order_release); }); }
		auto FState::AddPin(std::shared_ptr<const void> Pin) -> void
		{ std::lock_guard Lock(Mutex); Pins.push_back(std::move(Pin)); }
	}
}
