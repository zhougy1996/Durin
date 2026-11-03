#include "Threading/Future.h"
#include "Threading/Task.h"
#include <gtest/gtest.h>
#include <barrier>
#include <latch>

namespace Durin
{
	static_assert(!std::is_copy_constructible_v<TPromise<int>>);
	static_assert(!std::is_copy_constructible_v<TFuture<int>>);
	static_assert(std::is_copy_constructible_v<TSharedFuture<int>>);
	static_assert(!Tasks::Detail::CTaskDependency<TSharedFuture<int>>);
	template<typename T> concept CHasFutureContinuation = requires(T& Future) {
		Future.Then([](TFuture<int>) { return 1; });
	};
	template<typename T> concept CHasFutureReset = requires(T& Future) { Future.Reset(); };
	static_assert(CHasFutureContinuation<TFuture<int>>);
	static_assert(!CHasFutureContinuation<TSharedFuture<int>>);
	static_assert(CHasFutureReset<TFuture<int>>);
	static_assert(!CHasFutureReset<TSharedFuture<int>>);
	static_assert(std::is_convertible_v<TFuture<int>, TSharedFuture<int>>);

	TEST(FFutureTests, ThenTransfersConsumerAndRunsOnPublishingOrRegisteringThread)
	{
		TPromise<std::unique_ptr<int>> Promise;
		auto Source = Promise.GetFuture();
		std::thread::id CallbackThread;
		auto Result = Source.Then([&](TFuture<std::unique_ptr<int>> Input) {
			EXPECT_TRUE(Input.IsReady());
			EXPECT_EQ(Input.GetState(), EFutureState::Succeeded);
			CallbackThread = std::this_thread::get_id();
			return *Input.Consume() + 1;
		});
		EXPECT_FALSE(Source.IsValid());
		EXPECT_FALSE(Result.IsReady());
		std::thread Publisher([&] { Promise.SetValue(std::make_unique<int>(41)); });
		const auto PublisherId = Publisher.get_id();
		Publisher.join();
		EXPECT_EQ(Result.Get(), 42);
		EXPECT_EQ(CallbackThread, PublisherId);
		bool Called = false;
		const auto RegisteringThread = std::this_thread::get_id();
		auto Ready = MakeReadyFuture(7).Then([&](TFuture<int> Input) {
			EXPECT_EQ(std::this_thread::get_id(), RegisteringThread);
			Called = true;
			return Input.Get();
		});
		EXPECT_TRUE(Called);
		EXPECT_TRUE(Ready.IsReady());
		EXPECT_EQ(Ready.Get(), 7);
	}

	TEST(FFutureTests, NextConsumesMoveOnlyValuesAndChainsVoidWithoutScheduling)
	{
		ShutdownTaskScheduler(false);
		TPromise<std::unique_ptr<int>> Promise;
		auto Source = Promise.GetFuture();
		uint32 Calls = 0;
		auto Result = Source.Next([Extra = std::make_unique<int>(2)](std::unique_ptr<int> Value) {
			return *Value + *Extra;
		}).Next([&](int Value) { EXPECT_EQ(Value, 42); ++Calls; })
			.Next([&] { ++Calls; return std::make_unique<int>(43); });
		EXPECT_FALSE(Source.IsValid());
		EXPECT_FALSE(Result.IsReady());
		Promise.SetValue(std::make_unique<int>(40));
		EXPECT_EQ(*Result.Consume(), 43);
		EXPECT_EQ(Calls, 2u);
		EXPECT_FALSE(IsTaskSchedulerRunning());
		bool VoidCalled = false;
		auto Void = MakeReadyFuture().Then([&](TFuture<void> Input) {
			Input.GetMutable();
			Input.Consume();
			EXPECT_FALSE(Input.IsValid());
			VoidCalled = true;
		});
		Void.GetMutable();
		EXPECT_TRUE(VoidCalled);
	}

	TEST(FFutureTests, ThenCanRecoverEveryTerminalOutcomeAndReturnedFuturesRemainValues)
	{
		for (const auto Outcome : {EFutureState::Succeeded, EFutureState::Failed, EFutureState::Canceled})
		{
			TPromise<int> Promise;
			auto Result = Promise.GetFuture().Then([Outcome](TFuture<int> Input) {
				EXPECT_TRUE(Input.IsReady());
				EXPECT_EQ(Input.GetState(), Outcome);
				if (Outcome == EFutureState::Failed) EXPECT_EQ(Input.GetFailure().Code, EFutureFailure::Exception);
				return 42;
			});
			if (Outcome == EFutureState::Succeeded) Promise.SetValue(1);
			else if (Outcome == EFutureState::Canceled) Promise.SetCanceled();
			else Promise.SetFailure({});
			EXPECT_EQ(Result.Get(), 42);
		}
		TFuture<int> Recovered;
		{
			TPromise<int> Promise;
			Recovered = Promise.GetFuture().Then([](TFuture<int> Input) {
				EXPECT_EQ(Input.GetFailure().Code, EFutureFailure::Abandoned);
				return 17;
			});
		}
		EXPECT_EQ(Recovered.Get(), 17);
		auto Nested = MakeReadyFuture(1).Then([](TFuture<int> Input) { return MakeReadyFuture(Input.Get() + 1); });
		static_assert(std::is_same_v<decltype(Nested), TFuture<TFuture<int>>>);
		EXPECT_EQ(Nested.Consume().Consume(), 2);
	}

	TEST(FFutureTests, NextPreservesFailureCancellationAndAbandonmentWithoutCallingValueCode)
	{
		for (const auto Outcome : {EFutureState::Failed, EFutureState::Canceled})
		{
			TPromise<int> Promise;
			bool Called = false;
			auto Result = Promise.GetFuture().Next([&](int) { Called = true; return 42; });
			if (Outcome == EFutureState::Canceled) Promise.SetCanceled();
			else Promise.SetFailure({EFutureFailure::Exception, std::make_exception_ptr(std::out_of_range("input"))});
			EXPECT_EQ(Result.GetState(), Outcome);
			EXPECT_FALSE(Called);
			if (Outcome == EFutureState::Failed) EXPECT_THROW(Result.Get(), std::out_of_range);
			else EXPECT_THROW(Result.Get(), std::runtime_error);
		}
		TFuture<void> Result;
		bool Called = false;
		{
			TPromise<void> Promise;
			Result = Promise.GetFuture().Next([&] { Called = true; });
		}
		EXPECT_FALSE(Called);
		EXPECT_EQ(Result.GetFailure().Code, EFutureFailure::Abandoned);
	}

	TEST(FFutureTests, ContinuationExceptionsSettleDownstreamAndReleaseCaptures)
	{
		TPromise<int> Promise;
		auto Capture = std::make_shared<int>(1);
		std::weak_ptr<int> Weak = Capture;
		auto Result = Promise.GetFuture().Then([Capture = std::move(Capture)](TFuture<int>) -> int {
			throw std::out_of_range("continuation");
		});
		Promise.SetValue(1);
		EXPECT_TRUE(Weak.expired());
		EXPECT_EQ(Result.GetState(), EFutureState::Failed);
		EXPECT_THROW(Result.Get(), std::out_of_range);
		auto Void = MakeReadyFuture().Next([] { throw std::runtime_error("void continuation"); });
		EXPECT_EQ(Void.GetState(), EFutureState::Failed);
		EXPECT_THROW(Void.Get(), std::runtime_error);
		struct FThrowingMove { FThrowingMove() = default; FThrowingMove(FThrowingMove&&) { throw std::runtime_error("storage"); } };
		auto Storage = MakeReadyFuture().Next([] { return FThrowingMove{}; });
		EXPECT_EQ(Storage.GetState(), EFutureState::Failed);
		EXPECT_THROW(Storage.Get(), std::runtime_error);
	}

	TEST(FFutureTests, ContinuationRegistrationRacesPublicationWithoutLosingOrDuplicatingWork)
	{
		for (uint32 Iteration = 0; Iteration < 128; ++Iteration)
		{
			TPromise<int> Promise;
			auto Source = Promise.GetFuture();
			TFuture<int> Result;
			std::barrier Start(2);
			std::atomic<uint32> Calls = 0;
			std::thread Publisher([&] { Start.arrive_and_wait(); Promise.SetValue(41); });
			Start.arrive_and_wait();
			Result = Source.Next([&](int Value) { ++Calls; return Value + 1; });
			Publisher.join();
			EXPECT_TRUE(Result.IsReady());
			EXPECT_EQ(Result.Get(), 42);
			EXPECT_EQ(Calls.load(), 1u);
		}
	}

	TEST(FFutureTests, ResetAndDiscardDoNotCancelProducersOrAcceptedContinuations)
	{
		TPromise<int> Promise;
		auto Source = Promise.GetFuture();
		FFutureReadiness Ready;
		auto Observation = Source.ObserveReady(Ready);
		Source.Reset(); Source.Reset();
		EXPECT_FALSE(Source.IsValid());
		Promise.SetValue(1);
		EXPECT_TRUE(Ready.IsReady());
		TPromise<int> Pending;
		bool Called = false;
		auto Discarded = Pending.GetFuture().Next([&](int Value) { Called = true; EXPECT_EQ(Value, 42); });
		Discarded.Reset();
		Pending.SetValue(42);
		EXPECT_TRUE(Called);
		std::weak_ptr<int> Weak;
		{
			TPromise<void> Abandoned;
			auto Capture = std::make_shared<int>(1); Weak = Capture;
			Abandoned.GetFuture().Next([Capture = std::move(Capture)] {});
			EXPECT_FALSE(Weak.expired());
		}
		EXPECT_TRUE(Weak.expired());
	}

	TEST(FFutureTests, SharedConstructionAndConsumeTransferEvenInvalidAndFailedConsumers)
	{
		auto Source = MakeReadyFuture(std::make_unique<int>(42));
		TSharedFuture<std::unique_ptr<int>> Shared = std::move(Source);
		EXPECT_FALSE(Source.IsValid());
		auto Copy = Shared;
		EXPECT_EQ(&Shared.Get(), &Copy.Get());
		EXPECT_EQ(*Shared.Get(), 42);
		TFuture<int> Empty;
		TSharedFuture<int> Invalid = std::move(Empty);
		EXPECT_FALSE(Invalid.IsValid());
		EXPECT_FALSE(Empty.Share().IsValid());
		EXPECT_THROW(Empty.Then([](TFuture<int>) {}), std::logic_error);
		EXPECT_THROW(Empty.Next([](int) {}), std::logic_error);
		for (const auto Outcome : {EFutureState::Failed, EFutureState::Canceled})
		{
			TPromise<int> Promise;
			auto Value = Promise.GetFuture();
			if (Outcome == EFutureState::Canceled) Promise.SetCanceled(); else Promise.SetFailure({});
			EXPECT_THROW(Value.Consume(), std::runtime_error);
			EXPECT_FALSE(Value.IsValid());
		}
	}

	TEST(FFutureTests, ReferenceResultsPreserveIdentityAndNextUsesValueReturnDeduction)
	{
		int Value = 41;
		TPromise<int&> Promise;
		auto Source = Promise.GetFuture();
		Promise.SetValue(Value);
		EXPECT_EQ(&Source.Get(), &Value);
		auto Result = Source.Then([](TFuture<int&> Input) -> int& { return Input.Consume(); });
		static_assert(std::is_same_v<decltype(Result), TFuture<int&>>);
		Result.GetMutable() = 42;
		EXPECT_EQ(Value, 42);
		auto Copied = Result.Next([](int& Input) -> int& { return Input; });
		static_assert(std::is_same_v<decltype(Copied), TFuture<int>>);
		EXPECT_EQ(Copied.Get(), 42);
		EXPECT_NE(&Copied.Get(), &Value);
		auto Shared = MakeReadyFuture<const int&>(Value).Share();
		EXPECT_EQ(&Shared.Get(), &Value);
		EXPECT_EQ(Shared.GetResultShared().get(), &Value);
	}

	TEST(FFutureTests, WaitUntilHandlesWallAndSteadyDeadlinesAndReadyInputs)
	{
		TPromise<int> Promise;
		auto Future = Promise.GetFuture();
		EXPECT_FALSE(Future.WaitUntil(std::chrono::steady_clock::time_point::min()));
		EXPECT_FALSE(Future.WaitUntil(std::chrono::system_clock::now()));
		EXPECT_FALSE(Future.WaitUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
		std::thread Publisher([&] { Promise.SetValue(42); });
		EXPECT_TRUE(Future.WaitUntil(std::chrono::steady_clock::time_point::max()));
		Publisher.join();
		EXPECT_TRUE(Future.WaitUntil(std::chrono::system_clock::time_point::min()));
		EXPECT_EQ(Future.Get(), 42);
		EXPECT_TRUE(Future.Share().WaitUntil(std::chrono::system_clock::now()));
		EXPECT_THROW(Future.WaitUntil(std::chrono::steady_clock::now()), std::logic_error);
	}

	TEST(FFutureTests, MoveOnlyResultsShareAndRetainImmutableViews)
	{
		TPromise<std::unique_ptr<int>> Promise;
		auto Future = Promise.GetFuture();
		EXPECT_THROW(Promise.GetFuture(), std::logic_error);
		EXPECT_FALSE(Future.IsReady());
		EXPECT_FALSE(Future.WaitFor(std::chrono::nanoseconds(0)));
		Promise.SetValue(std::make_unique<int>(41));
		EXPECT_EQ(41, *Future.Get());
		EXPECT_EQ(&Future.Get(), &Future.Get());
		*Future.GetMutable() = 42;
		auto Shared = Future.Share();
		EXPECT_FALSE(Future.IsValid());
		const auto Copy = Shared;
		EXPECT_EQ(&Shared.Get(), &Copy.Get());
		auto View = Shared.GetResultShared();
		Shared = {}; Promise = {};
		EXPECT_EQ(42, **View);
	}

	TEST(FFutureTests, ConsumeInvalidatesAndVoidHasTheSameTerminalContract)
	{
		auto Value = MakeReadyFuture(std::make_unique<int>(17));
		EXPECT_EQ(17, *Value.Consume());
		EXPECT_FALSE(Value.IsValid());
		EXPECT_THROW(Value.Get(), std::logic_error);
		auto Void = MakeReadyFuture();
		Void.Get(); Void.Get(); Void.Consume();
		EXPECT_FALSE(Void.IsValid());
		TPromise<void> Promise;
		auto Shared = Promise.GetFuture().Share();
		Promise.SetValue(); Shared.Get();
		EXPECT_TRUE(Shared.WaitFor(std::chrono::hours(1)));
		EXPECT_FALSE(Promise.TrySetValue());
		EXPECT_THROW(Promise.SetValue(), std::logic_error);
	}

	TEST(FFutureTests, VoidPublicationPreservesEveryOutcomeAndRejectsLateValues)
	{
		for (auto Outcome : {EFutureState::Succeeded, EFutureState::Failed, EFutureState::Canceled})
		{
			TPromise<void> Promise;
			auto Future = Promise.GetFuture();
			if (Outcome == EFutureState::Succeeded) EXPECT_TRUE(Promise.TrySetValue());
			else if (Outcome == EFutureState::Failed) EXPECT_TRUE(Promise.TrySetFailure({}));
			else EXPECT_TRUE(Promise.TrySetCanceled());
			EXPECT_FALSE(Promise.TrySetValue());
			Future.Wait(); EXPECT_EQ(Outcome, Future.GetState());
		}
	}

	TEST(FFutureTests, FailureAbandonmentCancellationAndMoveAssignmentWakeConsumers)
	{
		TFuture<int> Abandoned;
		{ TPromise<int> Promise; Abandoned = Promise.GetFuture(); }
		ASSERT_TRUE(Abandoned.IsReady());
		EXPECT_EQ(EFutureFailure::Abandoned, Abandoned.GetFailure().Code);
		EXPECT_THROW(Abandoned.Get(), std::runtime_error);
		TPromise<int> Replaced;
		auto Old = Replaced.GetFuture();
		TPromise<int> Incoming;
		auto New = Incoming.GetFuture();
		Replaced = std::move(Incoming);
		EXPECT_EQ(EFutureFailure::Abandoned, Old.GetFailure().Code);
		Replaced.SetValue(9); EXPECT_EQ(9, New.Get());
		EXPECT_THROW(Incoming.GetFuture(), std::logic_error);
		TPromise<void> Canceled;
		auto C = Canceled.GetFuture(); Canceled.SetCanceled();
		EXPECT_EQ(EFutureState::Canceled, C.GetState());
		EXPECT_THROW(C.Get(), std::runtime_error);
		TPromise<int> Failed;
		auto F = Failed.GetFuture();
		Failed.SetFailure({EFutureFailure::Exception, std::make_exception_ptr(std::out_of_range("producer"))});
		EXPECT_THROW(F.Get(), std::out_of_range);
		EXPECT_FALSE(Failed.TrySetCanceled());
		EXPECT_FALSE(Failed.TrySetValue(1));
		TPromise<std::expected<int, std::string>> Domain;
		auto D = Domain.GetFuture(); Domain.SetValue(std::unexpected("domain"));
		EXPECT_EQ(EFutureState::Succeeded, D.GetState());
		EXPECT_EQ("domain", D.Get().error());
	}

	TEST(FFutureTests, ThrowingStorageAndConsumeDoNotLeavePendingOrReusableConsumers)
	{
		struct FThrow
		{
			FThrow() = default;
			FThrow(FThrow&&) { throw std::runtime_error("move"); }
		};
		TPromise<FThrow> Promise;
		auto Future = Promise.GetFuture();
		EXPECT_TRUE(Promise.TrySetValue(FThrow{}));
		EXPECT_EQ(EFutureState::Failed, Future.GetState());
		EXPECT_THROW(Future.Get(), std::runtime_error);
		TPromise<FThrow> Checked;
		auto CheckedFuture = Checked.GetFuture();
		EXPECT_NO_THROW(Checked.SetValue(FThrow{}));
		EXPECT_EQ(EFutureState::Failed, CheckedFuture.GetState());
		EXPECT_THROW(CheckedFuture.Get(), std::runtime_error);
		struct FConsume
		{
			std::shared_ptr<bool> Throw;
			explicit FConsume(std::shared_ptr<bool> Flag) : Throw(std::move(Flag)) {}
			FConsume(FConsume&& Other) : Throw(Other.Throw) { if (*Throw) throw std::runtime_error("consume"); }
		};
		auto Flag = std::make_shared<bool>(false);
		auto Value = MakeReadyFuture(FConsume(Flag));
		*Flag = true;
		EXPECT_THROW(Value.Consume(), std::runtime_error);
		EXPECT_FALSE(Value.IsValid());
	}

	TEST(FFutureTests, PublicationSubscriptionAndCompetingSettersRaceWithoutLostNotification)
	{
		for (int Iteration = 0; Iteration < 128; ++Iteration)
		{
			TPromise<int> Promise;
			auto Future = Promise.GetFuture().Share();
			std::barrier Start(3);
			std::atomic<int> Calls = 0, Winners = 0;
			FFutureSubscription Observation;
			std::thread Publisher([&] { Start.arrive_and_wait(); Winners += Promise.TrySetValue(5); });
			std::thread Observer([&] {
				Start.arrive_and_wait();
				Observation = Future.Subscribe([&](EFutureState State) {
					EXPECT_NE(EFutureState::Pending, State); EXPECT_TRUE(Future.IsReady());
					if (State == EFutureState::Succeeded) EXPECT_EQ(5, *Future.GetResultShared());
					++Calls;
				});
			});
			Start.arrive_and_wait(); Winners += Promise.TrySetCanceled();
			Publisher.join(); Observer.join();
			EXPECT_EQ(1, Winners.load()); EXPECT_EQ(1, Calls.load());
		}
	}

	TEST(FFutureTests, DetachmentReentrancyAndInFlightNotificationHaveDefinedOwnership)
	{
		TPromise<void> Promise;
		auto Future = Promise.GetFuture().Share();
		int DetachedCalls = 0, ReentrantCalls = 0;
		auto Detached = Future.Subscribe([&](EFutureState) { ++DetachedCalls; });
		Detached.Reset();
		FFutureSubscription Nested;
		auto Reentrant = Future.Subscribe([&](EFutureState) {
			Future.Get(); Nested = Future.Subscribe([&](EFutureState) { ++ReentrantCalls; });
			throw std::runtime_error("contained");
		});
		FFutureReadiness Ready;
		EXPECT_FALSE(Ready.IsReady());
		auto Marker = Future.ObserveReady(Ready);
		std::latch Entered(1), Exit(1);
		auto Capture = std::make_shared<int>(1);
		std::weak_ptr<int> Weak = Capture;
		auto InFlight = Future.Subscribe([&, Capture](EFutureState) { Entered.count_down(); Exit.wait(); });
		Capture.reset();
		std::thread Publisher([&] { Promise.SetValue(); });
		Entered.wait(); InFlight.Reset();
		EXPECT_FALSE(Weak.expired());
		Exit.count_down(); Publisher.join();
		EXPECT_TRUE(Weak.expired()); EXPECT_TRUE(Ready.IsReady());
		EXPECT_EQ(0, DetachedCalls); EXPECT_EQ(1, ReentrantCalls);
		Future.Get(); // Detaching an observer does not cancel the producer's result.
		FFutureReadiness LateReady;
		auto LateMarker = Future.ObserveReady(LateReady);
		EXPECT_TRUE(LateReady.IsReady());
		auto Late = Future.Subscribe([&](EFutureState State) {
			EXPECT_EQ(EFutureState::Succeeded, State); ++ReentrantCalls;
		});
		EXPECT_EQ(2, ReentrantCalls);
	}

	TEST(FFutureTests, ResultViewsKeepPinsAfterAllHandlesAndProducerDisappear)
	{
		std::shared_ptr<const std::unique_ptr<int>> View;
		std::weak_ptr<int> Weak;
		{
			TPromise<std::unique_ptr<int>> Promise;
			auto Pin = std::make_shared<int>(7); Weak = Pin;
			auto Future = Promise.GetFuture().Share().WithLifetime(Pin);
			EXPECT_FALSE(Future.GetResultShared());
			Promise.SetValue(std::make_unique<int>(11));
			View = Future.GetResultShared();
		}
		EXPECT_FALSE(Weak.expired()); EXPECT_EQ(11, **View);
		View.reset(); EXPECT_TRUE(Weak.expired());

		// Pins can be attached after publication; the producer also retains the state.
		TPromise<int> Promise;
		auto Future = Promise.GetFuture().Share();
		Promise.SetValue(13);
		auto Pin = std::make_shared<int>(7); Weak = Pin;
		auto LateView = Future.WithLifetime(std::move(Pin)).GetResultShared();
		Future = {};
		EXPECT_EQ(13, *LateView);
		LateView.reset(); EXPECT_FALSE(Weak.expired());
		Promise = TPromise<int>{}; EXPECT_TRUE(Weak.expired());
	}

	TEST(FFutureTests, TimedWaitAndAbandonmentWakeBlockedConsumers)
	{
		std::optional<TPromise<int>> Promise(std::in_place);
		auto Future = Promise->GetFuture();
		EXPECT_FALSE(Future.WaitFor(std::chrono::milliseconds(1)));
		std::latch Started(1);
		std::thread Waiter([&] { Started.count_down(); Future.Wait(); EXPECT_EQ(EFutureState::Failed, Future.GetState()); });
		Started.wait(); Promise.reset(); Waiter.join();
	}

	TEST(FFutureTests, FuturesRemainIndependentAcrossSchedulerLifetimes)
	{
		ShutdownTaskScheduler(false);
		TPromise<int> Before;
		auto Retained = Before.GetFuture().Share();
		Before.SetValue(23);
		for (int Cycle = 0; Cycle < 2; ++Cycle)
		{
			ASSERT_TRUE(InitializeTaskScheduler(1));
			const auto BeforeCounts = GetTaskSchedulerDiagnostics();
			TPromise<void> During;
			auto Future = During.GetFuture();
			const auto AfterCounts = GetTaskSchedulerDiagnostics();
			EXPECT_EQ(BeforeCounts.NonterminalTaskCount, AfterCounts.NonterminalTaskCount);
			for (size_t Executor = 0; Executor < BeforeCounts.ExecutorStorage.size(); ++Executor)
				EXPECT_EQ(BeforeCounts.ExecutorStorage[Executor].CurrentPendingNodes,
					AfterCounts.ExecutorStorage[Executor].CurrentPendingNodes);
			// An unresolved future must not add a pending scheduler node or obstruct drain.
			ShutdownTaskScheduler(false);
			During.SetValue(); Future.Get();
			EXPECT_EQ(23, Retained.Get());
		}
	}

}
