#pragma once

#include "Misc/CoreStd.h"
#include "CoreAPI.h"

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
			CORE_API auto Claim() -> bool;
			CORE_API auto Publish(EFutureState Outcome, FFutureFailure Failure = {}) -> void;
		private:
			mutable std::mutex Mutex;
			mutable std::condition_variable Changed;
			EFutureState State = EFutureState::Pending;
			FFutureFailure Failure;
			bool Claimed = false;
			std::vector<std::weak_ptr<std::function<void(EFutureState)>>> Observers;
			// Destroyed after derived result storage, so deleters retain their ownership pins.
			std::vector<std::shared_ptr<const void>> Pins;
		};
		template<typename T> struct TState : FState { std::optional<T> Value; };
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
			auto GetFailure() const -> FFutureFailure { return Checked()->GetFailure(); }
			auto Get() const -> std::add_lvalue_reference_t<const T>
			{
				Checked()->CheckSuccess();
				if constexpr (!std::is_void_v<T>) return *State->Value;
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
		auto Share() -> TSharedFuture<T> { this->Checked(); return TSharedFuture<T>(std::move(this->State)); }
		auto GetMutable() -> std::add_lvalue_reference_t<T> requires (!std::is_void_v<T>)
		{ this->Checked()->CheckSuccess(); return *this->State->Value; }
		auto Consume() -> T
		{
			this->Checked()->CheckSuccess();
			auto Owned = std::move(this->State);
			if constexpr (!std::is_void_v<T>) return std::move(*Owned->Value);
		}
	private:
		friend class TPromise<T>;
		explicit TFuture(std::shared_ptr<FuturePrivate::TState<T>> State) : Super(std::move(State)) {}
	};

	template<typename T> class TSharedFuture : public FuturePrivate::TConsumer<T>
	{
		using Super = FuturePrivate::TConsumer<T>;
	public:
		TSharedFuture() = default;
		auto GetResultShared() const -> std::shared_ptr<const T> requires (!std::is_void_v<T>)
		{
			if (this->GetState() != EFutureState::Succeeded) return {};
			return {this->State, &*this->State->Value};
		}
		auto WithLifetime(std::shared_ptr<const void> Pin) const -> TSharedFuture
		{ this->Checked()->AddPin(std::move(Pin)); return *this; }
	private:
		friend class TFuture<T>;
		explicit TSharedFuture(std::shared_ptr<FuturePrivate::TState<T>> State) : Super(std::move(State)) {}
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
			try { State->Value.emplace(std::move(Value)); }
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

	template<typename T> auto MakeReadyFuture(T Value) -> TFuture<T>
	{
		TPromise<T> Promise;
		auto Future = Promise.GetFuture();
		Promise.SetValue(std::move(Value));
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
