#include "Threading/Future.h"
#include "Threading/TaskComposition.h"
#include <gtest/gtest.h>
#include <barrier>
#include <latch>

namespace Durin
{
	static_assert(!std::is_copy_constructible_v<TPromise<int>>);
	static_assert(!std::is_copy_constructible_v<TFuture<int>>);
	static_assert(std::is_copy_constructible_v<TSharedFuture<int>>);
	static_assert(!Tasks::Detail::CTaskDependency<TSharedFuture<int>>);

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
					EXPECT_NE(EFutureState::Pending, State); EXPECT_TRUE(Future.IsReady()); ++Calls;
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
	TEST(FFutureTests, StandaloneCompletionRetainsValueAndLifetimeWithoutScheduler)
	{
		auto Promise = TPromise<std::string>{};
		auto Future = Promise.GetFuture().Share();
		EXPECT_FALSE(Future.IsReady());
		EXPECT_FALSE(Future.GetResultShared());
		int Calls = 0;
		FFutureReadiness Signal;
		auto Observation = Future.ObserveReady(Signal);
		EXPECT_FALSE(Signal.IsReady());
		auto Subscription = Future.Subscribe([&](EFutureState State) { EXPECT_EQ(EFutureState::Succeeded, State); ++Calls; });
		EXPECT_TRUE(Promise.TrySetValue("owned"));
		EXPECT_FALSE(Promise.TrySetValue("duplicate"));
		ASSERT_TRUE(Future.IsReady());
		EXPECT_EQ(1, Calls);
		EXPECT_TRUE(Signal.IsReady());
		auto Lifetime = std::make_shared<int>(7);
		std::weak_ptr<int> Weak = Lifetime;
		auto Retained = Future.WithLifetime(std::move(Lifetime)).GetResultShared();
		Future = {};
		EXPECT_FALSE(Weak.expired());
		EXPECT_EQ("owned", *Retained);
		Retained.reset();
		EXPECT_FALSE(Weak.expired()); // The promise still retains the state.
		Promise = TPromise<std::string>{};
		EXPECT_TRUE(Weak.expired());
		auto Ready = MakeReadyFuture(42).Share();
		auto Late = Ready.Subscribe([&](EFutureState) { ++Calls; });
		EXPECT_EQ(2, Calls);
		EXPECT_EQ(42, *Ready.GetResultShared());
	}

	TEST(FFutureTests, StandaloneCompletionAbandonmentAndUnsubscribeDoNotCancelDomainWork)
	{
		TSharedFuture<int> Abandoned;
		{
			auto Promise = TPromise<int>{};
			Abandoned = Promise.GetFuture().Share();
		}
		EXPECT_EQ(EFutureState::Failed, Abandoned.GetState());
		EXPECT_EQ(EFutureFailure::Abandoned, Abandoned.GetFailure().Code);
		auto Promise = TPromise<int>{};
		auto Future = Promise.GetFuture().Share();
		int Calls = 0;
		auto Subscription = Future.Subscribe([&](EFutureState) { ++Calls; });
		Subscription.Reset();
		EXPECT_TRUE(Promise.TrySetValue(9));
		EXPECT_EQ(0, Calls);
		EXPECT_EQ(9, *Future.GetResultShared());
	}

	TEST(FFutureTests, StandaloneSubscriptionCompletionRaceNotifiesExactlyOnce)
	{
		for (int Iteration = 0; Iteration < 128; ++Iteration)
		{
			auto Promise = TPromise<int>{};
			auto Future = Promise.GetFuture().Share();
			std::atomic<int> Calls{0};
			std::thread Producer([Promise = std::move(Promise)] { EXPECT_TRUE(Promise.TrySetValue(1)); });
			auto Subscription = Future.Subscribe([&](EFutureState) { ++Calls; });
			Producer.join();
			EXPECT_EQ(1, Calls.load());
			EXPECT_EQ(1, *Future.GetResultShared());
		}
	}

}
