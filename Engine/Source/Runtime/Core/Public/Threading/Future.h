#pragma once

#include "Misc/CoreStd.h"
#include "CoreAPI.h"
#include "Templates/MoveOnlyFunction.h"

namespace Durin
{
	enum class EFutureState { Invalid, Pending, Succeeded, Failed, Canceled };
	enum class EFutureFailure { Exception, Abandoned };
	struct FFutureFailure
	{
		EFutureFailure Code = EFutureFailure::Exception;
		std::exception_ptr Exception;
	};

	namespace FuturePrivate { class FState; }
	// Reset detaches notifications that have not been pinned; an in-flight call may finish.
	class FFutureSubscription
	{
	public:
		FFutureSubscription() = default;
		FFutureSubscription(FFutureSubscription&&) noexcept = default;
		auto operator=(FFutureSubscription&&) noexcept -> FFutureSubscription& = default;
		FFutureSubscription(const FFutureSubscription&) = delete;
		auto operator=(const FFutureSubscription&) -> FFutureSubscription& = delete;
		auto Reset() -> void { Callback.reset(); }
	private:
		friend class FuturePrivate::FState;
		std::shared_ptr<std::function<void(EFutureState)>> Callback;
	};

	class FFutureReadiness
	{
	public:
		CORE_API FFutureReadiness();
		auto IsReady() const -> bool { return State->load(std::memory_order_acquire); }
	private:
		friend class FuturePrivate::FState;
		std::shared_ptr<std::atomic<bool>> State;
	};

	namespace FuturePrivate
	{
		// No task handles, executor dependencies, or scheduler admission live in this state.
		class FState
		{
		public:
			CORE_API auto GetState() const -> EFutureState;
			CORE_API auto Wait() const -> void;
			CORE_API auto WaitFor(std::chrono::steady_clock::duration Timeout) const -> bool;
			CORE_API auto CheckSuccess() const -> void;
			CORE_API auto GetFailure() const -> FFutureFailure;
			CORE_API auto Subscribe(std::function<void(EFutureState)> Callback) -> FFutureSubscription;
			CORE_API auto ObserveReady(const FFutureReadiness& Readiness) -> FFutureSubscription;
			CORE_API auto AddPin(std::shared_ptr<const void> Pin) -> void;
			CORE_API auto SetContinuation(TMoveOnlyFunction<void()>&& Callback) -> void;
			CORE_API auto Claim() -> bool;
			CORE_API auto Publish(EFutureState Outcome, FFutureFailure Failure = {}) -> void;
		private:
			mutable std::mutex Mutex;
			mutable std::condition_variable Changed;
			EFutureState State = EFutureState::Pending;
			FFutureFailure Failure;
			bool Claimed = false;
			std::vector<std::weak_ptr<std::function<void(EFutureState)>>> Observers;
			TMoveOnlyFunction<void()> Continuation;
			// Destroyed after derived result storage, so deleters retain their ownership pins.
			std::vector<std::shared_ptr<const void>> Pins;
		};
		template<typename T> struct TState : FState
		{
			std::optional<T> Value;
			auto GetValue() -> T& { return *Value; }
		};
		template<typename T> struct TState<T&> : FState
		{
			std::optional<std::reference_wrapper<T>> Value;
			auto GetValue() -> T& { return Value->get(); }
		};
		template<> struct TState<void> : FState {};

		template<typename T> class TConsumer
		{
		public:
			auto IsValid() const -> bool { return !!State; }
			auto GetState() const -> EFutureState { return State ? State->GetState() : EFutureState::Invalid; }
			auto IsReady() const -> bool { const auto S = GetState(); return S != EFutureState::Invalid && S != EFutureState::Pending; }
			auto Wait() const -> void { Checked()->Wait(); }
			template<typename Rep, typename Period>
			auto WaitFor(std::chrono::duration<Rep, Period> Timeout) const -> bool
			{
				// Clamp before converting to avoid overflow for very large durations.
				const auto Seconds = std::chrono::duration<long double>(Timeout).count();
				const auto Limit = std::chrono::duration<long double>(std::chrono::steady_clock::duration::max()).count();
				return Checked()->WaitFor(!(Seconds > 0) ? std::chrono::steady_clock::duration::zero()
					: Seconds >= Limit ? std::chrono::steady_clock::duration::max()
					: std::chrono::duration_cast<std::chrono::steady_clock::duration>(Timeout));
			}
			template<typename Clock, typename Duration>
			auto WaitUntil(std::chrono::time_point<Clock, Duration> Deadline) const -> bool
			{
				// Subtract in a wide duration so min/max time points cannot overflow.
				return WaitFor(std::chrono::duration<long double>(Deadline.time_since_epoch())
					- std::chrono::duration<long double>(Clock::now().time_since_epoch()));
			}
			auto GetFailure() const -> FFutureFailure { return Checked()->GetFailure(); }
			auto Get() const -> std::add_lvalue_reference_t<const T>
			{
				Checked()->CheckSuccess();
				if constexpr (!std::is_void_v<T>) return State->GetValue();
			}
			auto Subscribe(std::function<void(EFutureState)> Callback) const -> FFutureSubscription
			{ return Checked()->Subscribe(std::move(Callback)); }
			auto ObserveReady(const FFutureReadiness& Readiness) const -> FFutureSubscription
			{ return Checked()->ObserveReady(Readiness); }
		protected:
			TConsumer() = default;
			explicit TConsumer(std::shared_ptr<TState<T>> InState) : State(std::move(InState)) {}
			auto Checked() const -> const std::shared_ptr<TState<T>>&
			{
				if (!State) throw std::logic_error("Invalid future.");
				return State;
			}
			std::shared_ptr<TState<T>> State;
		};
	}

	template<typename T> class TPromise;
	template<typename T> class TSharedFuture;
	template<typename T> class TFuture : public FuturePrivate::TConsumer<T>
	{
		using Super = FuturePrivate::TConsumer<T>;
	public:
		TFuture() = default;
		TFuture(TFuture&&) noexcept = default;
		auto operator=(TFuture&&) noexcept -> TFuture& = default;
		TFuture(const TFuture&) = delete;
		auto operator=(const TFuture&) -> TFuture& = delete;
		auto Share() -> TSharedFuture<T> { return TSharedFuture<T>(std::move(*this)); }
		auto GetMutable() -> std::add_lvalue_reference_t<T>
		{
			this->Checked()->CheckSuccess();
			if constexpr (!std::is_void_v<T>) return this->State->GetValue();
		}
		auto Consume() -> T
		{
			TFuture Local(std::move(*this));
			if constexpr (std::is_void_v<T>) Local.Get();
			else if constexpr (std::is_reference_v<T>) return Local.GetMutable();
			else return std::move(Local.GetMutable());
		}
		// Transfers this consumer to the callback. Already-ready inputs run inline.
		template<typename F> auto Then(F Continuation);
		template<typename F> auto Next(F Continuation);
		auto Reset() -> void
		{
			if (auto Owned = std::move(this->State)) Owned->SetContinuation({});
		}
	private:
		friend class TPromise<T>;
		friend class TSharedFuture<T>;
		template<bool bValueOnly, typename F> auto Continue(F Continuation);
		explicit TFuture(std::shared_ptr<FuturePrivate::TState<T>> State) : Super(std::move(State)) {}
	};

	template<typename T> class TSharedFuture : public FuturePrivate::TConsumer<T>
	{
		using Super = FuturePrivate::TConsumer<T>;
	public:
		TSharedFuture() = default;
		TSharedFuture(TFuture<T>&& Future) : Super(std::move(Future.State)) {}
		auto GetResultShared() const -> std::shared_ptr<const std::remove_reference_t<T>> requires (!std::is_void_v<T>)
		{
			if (this->GetState() != EFutureState::Succeeded) return {};
			return {this->State, std::addressof(this->State->GetValue())};
		}
		auto WithLifetime(std::shared_ptr<const void> Pin) const -> TSharedFuture
		{ this->Checked()->AddPin(std::move(Pin)); return *this; }
	};

	template<typename T> class TPromise
	{
	public:
		TPromise() : State(std::make_shared<FuturePrivate::TState<T>>()) {}
		TPromise(TPromise&& Other) noexcept : State(std::move(Other.State)), Taken(Other.Taken) {}
		auto operator=(TPromise&& Other) noexcept -> TPromise&
		{
			if (this != &Other) { Abandon(); State = std::move(Other.State); Taken = Other.Taken; }
			return *this;
		}
		TPromise(const TPromise&) = delete;
		auto operator=(const TPromise&) -> TPromise& = delete;
		~TPromise() { Abandon(); }
		auto GetFuture() -> TFuture<T>
		{
			Checked();
			if (std::exchange(Taken, true)) throw std::logic_error("Future already retrieved.");
			return TFuture<T>(State);
		}
		auto TrySetValue() const -> bool requires std::is_void_v<T>
		{
			if (!Checked()->Claim()) return false;
			State->Publish(EFutureState::Succeeded);
			return true;
		}
		template<typename U = T> requires (std::same_as<T, U> && !std::is_void_v<U>)
		auto TrySetValue(std::type_identity_t<U> Value) const -> bool
		{ return TryPublishValue(Value); }
		auto TrySetFailure(FFutureFailure Failure) const -> bool
		{
			if (!Checked()->Claim()) return false;
			State->Publish(EFutureState::Failed, std::move(Failure));
			return true;
		}
		auto TrySetCanceled() const -> bool
		{
			if (!Checked()->Claim()) return false;
			State->Publish(EFutureState::Canceled);
			return true;
		}
		auto SetValue() const -> void requires std::is_void_v<T> { CheckPublication(TrySetValue()); }
		template<typename U = T> requires (std::same_as<T, U> && !std::is_void_v<U>)
		auto SetValue(std::type_identity_t<U> Value) const -> void { CheckPublication(TryPublishValue(Value)); }
		auto SetFailure(FFutureFailure Failure) const -> void { CheckPublication(TrySetFailure(std::move(Failure))); }
		auto SetCanceled() const -> void { CheckPublication(TrySetCanceled()); }
	private:
		// Both setters construct storage inside the publication exception boundary.
		// Forwarding by value between setters could throw before a publisher claims it.
		template<typename U> auto TryPublishValue(U& Value) const -> bool
		{
			if (!Checked()->Claim()) return false;
			try
			{
				if constexpr (std::is_reference_v<T>) State->Value.emplace(Value);
				else State->Value.emplace(std::move(Value));
			}
			catch (...) { State->Publish(EFutureState::Failed, {EFutureFailure::Exception, std::current_exception()}); return true; }
			State->Publish(EFutureState::Succeeded);
			return true;
		}
		static auto CheckPublication(bool Won) -> void { if (!Won) throw std::logic_error("Promise already resolved."); }
		auto Checked() const -> const std::shared_ptr<FuturePrivate::TState<T>>&
		{ if (!State) throw std::logic_error("Invalid promise."); return State; }
		auto Abandon() noexcept -> void
		{ if (State && State->Claim()) State->Publish(EFutureState::Failed, {EFutureFailure::Abandoned, {}}); }
		std::shared_ptr<FuturePrivate::TState<T>> State;
		bool Taken = false;
	};

	template<typename T>
	template<bool bValueOnly, typename F>
	auto TFuture<T>::Continue(F Continuation)
	{
		this->Checked();
		using R = std::invoke_result_t<F&, TFuture<T>>;
		TPromise<R> Promise;
		auto Result = Promise.GetFuture();
		TMoveOnlyFunction<void()> Callback = [Promise = std::move(Promise),
			Continuation = std::move(Continuation), State = this->State]() mutable {
			if constexpr (bValueOnly)
			{
				if (State->GetState() == EFutureState::Canceled)
				{ Promise.SetCanceled(); State.reset(); return; }
				if (State->GetState() == EFutureState::Failed)
				{ Promise.SetFailure(State->GetFailure()); State.reset(); return; }
			}
			try
			{
				if constexpr (std::is_void_v<R>)
				{
					std::invoke(Continuation, TFuture<T>(std::move(State)));
					Promise.SetValue();
				}
				else Promise.SetValue(std::invoke(Continuation, TFuture<T>(std::move(State))));
			}
			catch (...) { Promise.SetFailure({EFutureFailure::Exception, std::current_exception()}); }
		};
		auto Owned = std::move(this->State);
		Owned->SetContinuation(std::move(Callback));
		return Result;
	}

	template<typename T>
	template<typename F>
	auto TFuture<T>::Then(F Continuation)
	{ return Continue<false>(std::move(Continuation)); }

	template<typename T>
	template<typename F>
	auto TFuture<T>::Next(F Continuation)
	{
		return Continue<true>([Continuation = std::move(Continuation)](TFuture<T> Self) mutable {
			// Durin has terminal outcomes in addition to UE's value-only futures.
			// Keep cancellation and failure intact instead of invoking a value callback.
			if constexpr (std::is_void_v<T>)
			{
				Self.Consume();
				return std::invoke(Continuation);
			}
			else return std::invoke(Continuation, Self.Consume());
		});
	}

	template<typename T> auto MakeReadyFuture(T Value) -> TFuture<T>
	{
		TPromise<T> Promise;
		auto Future = Promise.GetFuture();
		Promise.SetValue(std::forward<T>(Value));
		return Future;
	}
	inline auto MakeReadyFuture() -> TFuture<void>
	{
		TPromise<void> Promise;
		auto Future = Promise.GetFuture();
		Promise.SetValue();
		return Future;
	}
}
