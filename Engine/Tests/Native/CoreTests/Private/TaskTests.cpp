#include <gtest/gtest.h>
#include "HAL/PlatformLTS.h"
#include "CoreGlobals.h"
#include "Threading/Task.h"
#include "Threading/Future.h"
#include "Threading/ThreadEvent.h"
#include "Threading/RunnableThread.h"
#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#endif

namespace Durin
{
	namespace
	{
		auto EnsureGameThreadForTaskTest() -> void
		{
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
		}
		class FEngineThreadPoolTestGuard
		{
		public:
			FEngineThreadPoolTestGuard() { EnsureGameThreadForTaskTest(); }
			~FEngineThreadPoolTestGuard() { ShutdownTaskScheduler(false); }
		};
		auto WaitOffGameThread(const Tasks::FTask& Task) -> FTaskWaitResult
		{
			FTaskWaitResult Result;
			std::thread Caller([&] { Result = Task.WaitDetailed(); });
			Caller.join();
			return Result;
		}
		// A configured ordinary task, never a manually settled task result source.
		template<typename T>
		auto LaunchOutcome(const Tasks::FTask& Gate, T Value, ETaskState Outcome = ETaskState::Succeeded) -> Tasks::TTask<T>
		{
			return Tasks::LaunchTask("ControlledOutcome", [Value = std::move(Value), Outcome]() mutable -> T {
				if (Outcome == ETaskState::Failed) throw std::runtime_error("expected task failure");
				if (Outcome == ETaskState::Canceled) Private::FTaskRuntimeAccess::SetCurrentCanceledOutcome();
				return std::move(Value);
			}, Gate);
		}
	}
	template<typename T> concept CHasTaskCancellationQuery = requires(const T& Task) { Task.IsCancellationRequested(); };
	static_assert(!CHasTaskCancellationQuery<Tasks::FTask>);
	static_assert(!CHasTaskCancellationQuery<Tasks::TTask<int>>);

	TEST(FCancellationTokenTests, CopiesSnapshotCancellationState)
	{
		Tasks::FCancellationToken Token;
		auto Snapshot = Token;
		EXPECT_FALSE(Token.IsCanceled());
		Token.Cancel();
		Token.Cancel();
		EXPECT_TRUE(Token.IsCanceled());
		EXPECT_FALSE(Snapshot.IsCanceled());
		Snapshot = Token;
		EXPECT_TRUE(Snapshot.IsCanceled());
		auto Moved = std::move(Snapshot);
		EXPECT_TRUE(Moved.IsCanceled());
	}

	static_assert(!std::is_same_v<Tasks::FTask, Tasks::TTask<void>>);
	static_assert(std::is_convertible_v<const Tasks::TTask<void>&, Tasks::FTask>);
	static_assert(!std::is_convertible_v<Tasks::TTask<void>&, Tasks::FTask&>);
	static_assert(std::is_copy_constructible_v<Tasks::TTask<void>>);
	static_assert(std::is_copy_constructible_v<Tasks::FTaskEvent>);
	static_assert(sizeof(Tasks::FTaskEvent) == sizeof(Tasks::FTask));
	static_assert(std::is_convertible_v<const Tasks::TTask<int>&, Tasks::FTask>);
	static_assert(!std::is_convertible_v<Tasks::TTask<int>&, Tasks::FTask&>);
	static_assert(std::derived_from<Tasks::FTaskEvent, Tasks::FTask>);
	static_assert(std::is_copy_constructible_v<Tasks::FTask>);
	static_assert(std::is_copy_assignable_v<Tasks::FTask>);
	static_assert(std::is_nothrow_move_constructible_v<Tasks::FTask>);
	static_assert(sizeof(Tasks::FTask) == sizeof(std::shared_ptr<FTaskStateData>));
	static_assert(!Tasks::Detail::CTaskDependency<TFuture<int>>);
	static_assert(!Tasks::Detail::CTaskDependency<TSharedFuture<int>>);
	static_assert(!Tasks::Detail::CTaskDependency<TPromise<int>>);
	static_assert(std::same_as<decltype(std::declval<Tasks::FTask>().Wait()), bool>);
	static_assert(std::same_as<decltype(std::declval<Tasks::TTask<int>>().Wait(0)), bool>);
	static_assert(std::same_as<decltype(Tasks::Wait(std::declval<const std::array<Tasks::FTask, 2>&>())), bool>);
	static_assert(std::same_as<decltype(Tasks::WaitAny(std::declval<const std::array<Tasks::FTask, 2>&>())), int32>);
	static_assert(std::same_as<decltype(std::declval<Tasks::TTask<int>>().WaitDetailed()), FTaskWaitResult>);
	static_assert(std::same_as<decltype(Tasks::LaunchTask("VoidHandleResult", [] { return Tasks::TTask<void>{}; })),
		Tasks::TTask<Tasks::TTask<void>>>);

	TEST(FTaskCompositionTests, ReturnedTaskHandlesAreValuesWithoutImplicitCompletionEdges)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (auto Outcome : {ETaskState::Succeeded, ETaskState::Failed, ETaskState::Canceled})
		{
			Tasks::FTaskEvent Gate("ReturnedHandleGate", false);
			struct FTrigger { Tasks::FTaskEvent& Event; ~FTrigger() { Event.Trigger(); } } Cleanup{Gate};
			auto Inner = LaunchOutcome(Gate, 42, Outcome);
			auto Outer = Tasks::LaunchTask("ReturnedHandleOuter", [Inner] { return Inner; });
			static_assert(std::same_as<decltype(Outer), Tasks::TTask<Tasks::TTask<int>>>);
			ASSERT_TRUE(Outer.Wait(2));
			EXPECT_EQ(Outer.GetState(), ETaskState::Succeeded);
			EXPECT_FALSE(Inner.IsCompleted());
			EXPECT_EQ(Outer.GetResult().GetTaskId(), Inner.GetTaskId());
			auto AfterOuter = Tasks::LaunchTask("ReturnedHandleDependent", [Inner] { return Inner.IsCompleted(); }, Outer);
			EXPECT_FALSE(AfterOuter.GetResult());
			Gate.Trigger();
			EXPECT_TRUE(Outer.GetResult().Wait(2));
			EXPECT_EQ(Outer.GetResult().GetState(), Outcome);
			EXPECT_EQ(Outer.GetState(), ETaskState::Succeeded);
			if (Outcome == ETaskState::Succeeded) EXPECT_EQ(Outer.GetResult().GetResult(), 42);
		}
	}

	TEST(FTaskCompositionTests, ReturnedTaskHandlesJoinOnlyThroughExplicitAddNested)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (auto Outcome : {ETaskState::Succeeded, ETaskState::Failed, ETaskState::Canceled})
		{
			Tasks::FTaskEvent Gate("NestedReturnedHandleGate", false);
			struct FTrigger { Tasks::FTaskEvent& Event; ~FTrigger() { Event.Trigger(); } } Cleanup{Gate};
			auto Inner = LaunchOutcome(Gate, 42, Outcome);
			FThreadEvent Registered;
			auto Outer = Tasks::LaunchTask("NestedReturnedHandleOuter", [Inner, &Registered] {
				Tasks::AddNested(Inner);
				Registered.Trigger();
				return Inner;
			});
			EXPECT_FALSE(Outer.Wait(0.01));
			EXPECT_TRUE(Registered.WaitFor(2));
			EXPECT_FALSE(Outer.IsCompleted());
			EXPECT_FALSE(Outer.GetResultShared());
			Gate.Trigger();
			ASSERT_TRUE(Outer.Wait(2));
			EXPECT_EQ(Outer.GetState(), ETaskState::Succeeded);
			EXPECT_EQ(Outer.GetResult().GetTaskId(), Inner.GetTaskId());
			EXPECT_EQ(Outer.GetResult().GetState(), Outcome);
		}
	}

	TEST(FTaskCompositionTests, ErasedOuterHandlesRetainReturnedTaskAndMoveOnlyResults)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<uint32> Destroyed = 0;
		struct FValue { std::atomic<uint32>& Count; ~FValue() { ++Count; } };
		auto Inner = Tasks::LaunchTask("RetainedInnerResult", [&] { return std::make_unique<FValue>(Destroyed); });
		auto Outer = Tasks::LaunchTask("RetainedOuterResult", [Inner] { return Inner; });
		ASSERT_TRUE(Outer.Wait(2));
		EXPECT_EQ(Outer.GetResult().GetTaskId(), Inner.GetTaskId());
		EXPECT_NE(Outer.GetResult().GetResult(), nullptr);
		Tasks::FTask Erased = std::move(Outer);
		EXPECT_FALSE(Outer.IsValid());
		Inner = {};
		ShutdownTaskScheduler(true);
		EXPECT_EQ(Destroyed.load(), 0u);
		Erased = {};
		EXPECT_EQ(Destroyed.load(), 1u);
	}

	TEST(FTaskWaitTests, OrdinaryWaitReportsCompletionIndependentlyOfOutcome)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_TRUE(Tasks::FTask{}.Wait());
		EXPECT_TRUE(Tasks::Wait(Tasks::TTask<int>{}, 0));
		EXPECT_TRUE(Tasks::Wait(std::array<Tasks::FTask, 0>{}, 0));
		Tasks::FTaskEvent Gate("BoolWaitGate", false);
		auto Success = LaunchOutcome(Gate, 42);
		auto Failed = LaunchOutcome(Gate, 1, ETaskState::Failed);
		auto Canceled = LaunchOutcome(Gate, 2, ETaskState::Canceled);
		EXPECT_FALSE(Success.Wait(0));
		EXPECT_FALSE(Tasks::Wait(Tasks::Prerequisites(Success, Failed, Canceled), 0));
		Gate.Trigger();
		EXPECT_TRUE(Tasks::Wait(Tasks::Prerequisites(Success, Failed, Canceled)));
		EXPECT_TRUE(Failed.Wait());
		EXPECT_TRUE(Tasks::Wait(Canceled));
		EXPECT_EQ(Failed.GetState(), ETaskState::Failed);
		EXPECT_EQ(Canceled.GetState(), ETaskState::Canceled);
		EXPECT_EQ(Success.GetResult(), 42);
		const auto Detailed = Tasks::WaitDetailed(Tasks::Prerequisites(Canceled, Tasks::FTask{}, Failed));
		ASSERT_EQ(Detailed.size(), 3u);
		EXPECT_EQ(Detailed[0].TaskState, ETaskState::Canceled);
		EXPECT_EQ(Detailed[1].TaskState, ETaskState::Invalid);
		EXPECT_EQ(Detailed[2].TaskState, ETaskState::Failed);
	}

	TEST(FTaskWaitTests, CollectionWaitHelpsLaterInputsThatReleaseEarlierOnes)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Parent = Tasks::LaunchTask("BoolWaitParent", [] {
			Tasks::FTaskEvent Signal("BoolWaitEarlierSignal", false);
			auto Child = Tasks::LaunchTask("BoolWaitLaterSignaler", [Signal]() mutable { Signal.Trigger(); });
			const auto Inputs = Tasks::Prerequisites(Signal, Child, Signal);
			return Tasks::Wait(Inputs, 2);
		});
		EXPECT_TRUE(Parent.GetResult());
	}

	TEST(FTaskWaitTests, CollectionWaitPinsInputsBeforeHelpingReentersTheCaller)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started, Release;
		struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } Resume{Release};
		auto Blocker = Tasks::LaunchTask("SnapshotOccupiedWorker", [&] { Started.Trigger(); Release.Wait(); });
		ASSERT_TRUE(Started.WaitFor(2));
		for (const bool bDetailed : {false, true})
		{
			std::vector<Tasks::FTask> Inputs;
			auto Child = Tasks::LaunchTask("SnapshotClearsCallerCollection", [&] { std::vector<Tasks::FTask>().swap(Inputs); });
			Inputs.push_back(Child);
			if (bDetailed)
			{
				const auto Results = Tasks::WaitDetailed(Inputs, 2);
				ASSERT_EQ(Results.size(), 1u);
				EXPECT_EQ(Results.front().WaitStatus, ETaskWaitStatus::Completed);
			}
			else EXPECT_TRUE(Tasks::Wait(Inputs, 2));
			EXPECT_TRUE(Inputs.empty());
			EXPECT_TRUE(Child.IsCompleted());
		}
		Release.Trigger();
		EXPECT_TRUE(Blocker.Wait());
	}

	TEST(FTaskWaitTests, OrdinaryWaitPreservesDeadlockAndOwnerThreadGuards)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("BoolSelfWaitGate", false), Unknown("BoolUnknownSignal");
		EXPECT_FALSE(Tasks::Wait(Tasks::Prerequisites(Unknown), 0.01));
		EXPECT_EQ(Unknown.WaitDetailed(0.01).WaitStatus, ETaskWaitStatus::UnsupportedThread);
		Tasks::TTask<bool> Self;
		Self = Tasks::LaunchTask("BoolSelfWait", [&] {
			EXPECT_FALSE(Self.Wait(0.01));
			EXPECT_EQ(Self.WaitDetailed(0.01).WaitStatus, ETaskWaitStatus::SelfWait);
			EXPECT_EQ(Tasks::WaitAny(Tasks::Prerequisites(Self), 0.01), INDEX_NONE);
			return Tasks::Wait(Tasks::Prerequisites(Self), 0.01);
		}, Gate);
		Gate.Trigger();
		EXPECT_FALSE(Self.GetResult());
		Unknown.Trigger();
	}

	TEST(FTaskWaitTests, WaitAnyReturnsIndicesForEmptyInvalidAndAlreadyCompletedInputs)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		Tasks::FTaskEvent Pending("WaitAnyPending", false), Ready("WaitAnyReady", false);
		Ready.Trigger();
		EXPECT_EQ(Tasks::WaitAny(std::array<Tasks::FTask, 0>{}), INDEX_NONE);
		EXPECT_EQ(Tasks::WaitAny(Tasks::Prerequisites(Pending, Tasks::FTask{}, Ready), 0), 1);
		EXPECT_EQ(Tasks::WaitAny(Tasks::Prerequisites(Pending, Ready, Ready), 0), 1);
		EXPECT_EQ(Tasks::WaitAny(std::span<const Tasks::FTask>{}, 0), INDEX_NONE);
		Pending.Trigger();
		EXPECT_EQ(Tasks::WaitAny(Tasks::Prerequisites(Pending, Ready)), 0);
	}

	TEST(FTaskWaitTests, WaitAnyDoesNotExecuteQueuedCandidatesOrCancelOnTimeout)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started, Release;
		struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } Resume{Release};
		auto Blocker = Tasks::LaunchTask("WaitAnyOccupiedWorker", [&] { Started.Trigger(); Release.Wait(); });
		ASSERT_TRUE(Started.WaitFor(2));
		std::atomic<uint32> Runs = 0;
		const std::array Inputs{
			Tasks::LaunchTask("WaitAnyQueuedFirst", [&] { ++Runs; return 1; }),
			Tasks::LaunchTask("WaitAnyQueuedSecond", [&] { ++Runs; return 2; })};
		EXPECT_EQ(Tasks::WaitAny(Inputs, 0), INDEX_NONE);
		EXPECT_EQ(Tasks::WaitAny(Inputs, 0.01), INDEX_NONE);
		EXPECT_EQ(Runs.load(), 0u);
		Release.Trigger();
		EXPECT_TRUE(Tasks::Wait(Inputs, 2));
		EXPECT_TRUE(Blocker.Wait());
		EXPECT_EQ(Runs.load(), 2u);
	}

	TEST(FTaskWaitTests, WaitAnyObservesFailureAndCancellationAsCompletion)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (auto Outcome : {ETaskState::Failed, ETaskState::Canceled})
		{
			Tasks::FTaskEvent Pending("WaitAnyUnfinished", false), Gate("WaitAnyOutcomeGate", false);
			auto Work = LaunchOutcome(Gate, 1, Outcome);
			Gate.Trigger();
			EXPECT_EQ(Tasks::WaitAny(Tasks::Prerequisites(Pending, Work), 2), 1);
			EXPECT_EQ(Work.GetState(), Outcome);
			EXPECT_FALSE(Pending.IsCompleted());
			Pending.Trigger();
		}
	}

	TEST(FTaskWaitTests, WaitAnyCannotReturnBeforeTerminalHooksRetire)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		Tasks::FTaskEvent Signal("WaitAnyPublication", false);
		FThreadEvent Entered, Release;
		auto Hook = std::make_shared<Private::FTaskBeforeCompletionHook>();
		Hook->Function = [&](ETaskState) { Entered.Trigger(); Release.Wait(); };
		Private::FTaskRuntimeAccess::BindBeforeCompletion(Signal, std::move(Hook));
		std::thread Publisher([&] { Signal.Trigger(); });
		EXPECT_TRUE(Entered.WaitFor(2));
		EXPECT_EQ(Tasks::WaitAny(Tasks::Prerequisites(Signal), 0.01), INDEX_NONE);
		EXPECT_FALSE(Signal.IsCompleted());
		Release.Trigger();
		Publisher.join();
		EXPECT_EQ(Tasks::WaitAny(Tasks::Prerequisites(Signal), 0), 0);
	}

	TEST(FTaskWaitTests, WaitAnyRegistrationRacesPublicationWithoutLostWakeups)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		for (uint32 Iteration = 0; Iteration < 64; ++Iteration)
		{
			std::array Events{Tasks::FTaskEvent("WaitAnyRaceFirst", false), Tasks::FTaskEvent("WaitAnyRaceSecond", false)};
			FThreadEvent Start;
			std::thread Publisher([&] { Start.Wait(); Events[1].Trigger(); });
			Start.Trigger();
			EXPECT_EQ(Tasks::WaitAny(Events, 1), 1);
			Publisher.join();
			Events[0].Trigger();
		}
	}

	TEST(FTaskCompositionTests, ErasedHandleReplacementPreservesTypedResultBinding)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Typed = Tasks::LaunchTask("TypedBinding", [] { return std::make_unique<int>(42); });
		auto Copy = Typed;
		Tasks::FTask Erased = Typed;
		Erased = Tasks::LaunchTask("Replacement", [] {});
		ASSERT_EQ(Erased.WaitDetailed().WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_EQ(*Typed.GetResult(), 42);
		EXPECT_EQ(&Typed.GetResult(), &Copy.GetResult());
		auto View = Typed.GetResultShared();
		Erased = std::move(Typed);
		EXPECT_FALSE(Typed.IsValid());
		Copy = {};
		Erased = {};
		EXPECT_EQ(**View, 42);
	}

	TEST(FTaskCompositionTests, SavedExecutionOptionsOwnPrerequisitesAcrossCopyAndMove)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent First("SavedOptionsFirst", false), Second("SavedOptionsSecond", false);
		Tasks::FTaskExecutionOptions Options{.Prerequisites = {First}};
		{
			const auto Temporary = Tasks::Prerequisites(First, Second);
			Options.Prerequisites.assign(Temporary.begin(), Temporary.end());
		}
		auto Copied = Options;
		Options.Prerequisites.clear();
		auto Moved = std::move(Copied);
		std::atomic<uint32> Runs = 0;
		auto Task = Tasks::LaunchTask("SavedOptionsConsumer", [&] { ++Runs; return 42; }, std::move(Moved));
		EXPECT_EQ(Task.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
		First.Trigger();
		EXPECT_EQ(Task.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_EQ(Runs.load(), 0u);
		Second.Trigger();
		EXPECT_EQ(Task.GetResult(), 42);
		EXPECT_EQ(Runs.load(), 1u);
	}

	TEST(FTaskCompositionTests, WaitAllProgressesEveryInputOnASingleWorker)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Event("WaitAllProgress", false);
		FThreadEvent Done;
		auto Parent = Tasks::LaunchTask("WaitAllParent", [&] {
			auto First = Tasks::LaunchTask("WaitAllNested", [&] { Tasks::AddNested(Event); });
			auto Second = Tasks::LaunchTask("WaitAllSignal", [&] { Event.Trigger(); });
			const std::array<Tasks::FTask, 4> Inputs{First, Second, First, Tasks::FTask{}};
			const auto Results = WaitAll(Inputs, 0.1);
			ASSERT_EQ(Inputs.size(), Results.size());
			for (const auto& Result : Results) EXPECT_EQ(ETaskWaitStatus::Completed, Result.WaitStatus);
			EXPECT_EQ(ETaskState::Invalid, Results.back().TaskState);
			Event.Trigger();
			Done.Trigger();
		});
		EXPECT_TRUE(Done.WaitFor(5));
		EXPECT_EQ(ETaskState::Succeeded, Parent.WaitDetailed().TaskState);
		ShutdownTaskScheduler(true);
	}

	TEST(FTaskCompositionTests, WaitAllPreservesPollingAndIndividualRestrictions)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("WaitAllGate", false);
		int Runs = 0;
		auto Ready = Tasks::LaunchTask("WaitAllReady", [&] { ++Runs; }, Gate);
		const std::array<Tasks::FTask, 2> Polled{Ready, {}};
		const auto Poll = WaitAll(Polled, 0);
		EXPECT_EQ(ETaskWaitStatus::TimedOut, Poll[0].WaitStatus);
		EXPECT_EQ(ETaskWaitStatus::Completed, Poll[1].WaitStatus);
		EXPECT_EQ(0, Runs);
		Gate.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, Ready.WaitDetailed().TaskState);

		Tasks::FTaskEvent Unknown("WaitAllUnknown");
		auto Failed = Tasks::LaunchTask("WaitAllFailure", [] { throw std::runtime_error("expected"); });
		const std::array<Tasks::FTask, 3> Inputs{Unknown, Failed, Ready};
		const auto Results = WaitAll(Inputs, 2);
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Results[0].WaitStatus);
		EXPECT_EQ(ETaskWaitStatus::Completed, Results[1].WaitStatus);
		EXPECT_EQ(ETaskState::Failed, Results[1].TaskState);
		EXPECT_EQ(ETaskState::Succeeded, Results[2].TaskState);
		Unknown.Trigger();
	}

	TEST(FTaskCompositionTests, OveralignedMoveOnlyResultRetainsAddressAndDestroysOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<int> Destroyed = 0;
		struct alignas(128) FValue
		{
			std::unique_ptr<int> Payload;
			std::atomic<int>* Destroyed;
			explicit FValue(std::atomic<int>& Count) : Payload(std::make_unique<int>(42)), Destroyed(&Count) {}
			FValue(FValue&&) noexcept = default;
			auto operator=(FValue&&) -> FValue& = delete;
			~FValue()
			{
				if (Payload) { (void)GetTaskSchedulerDiagnostics(); ++*Destroyed; }
			}
		};
		auto Task = Tasks::LaunchTask("InlineAlignedResult", [&] { return FValue(Destroyed); });
		auto* Address = &Task.GetResult();
		EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(Address) % alignof(FValue));
		EXPECT_EQ(42, *Address->Payload);
		auto View = Task.GetResultShared();
		Tasks::FTask Generic = Task;
		ShutdownTaskScheduler(true);
		Task = {};
		EXPECT_EQ(Address, View.get());
		View.reset();
		EXPECT_EQ(0, Destroyed.load());
		Generic = {};
		EXPECT_EQ(1, Destroyed.load());
	}

	TEST(FTaskCompositionTests, ExecutionDependenciesRetainPendingResultsAndReleaseBeforeTheBody)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (bool AlreadyCompleted : {false, true})
		{
			FThreadEvent Started, Release;
			struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } Resume{Release};
			auto Blocker = Tasks::LaunchTask("DependencyLifetimeBlocker", [&] { Started.Trigger(); Release.Wait(); });
			ASSERT_TRUE(Started.WaitFor(2));
			Tasks::FTaskEvent Ready("ResultReady", false), Gate("DependencyLifetimeGate", false);
			std::weak_ptr<int> Weak;
			Tasks::TTask<void> Dependent;
			FThreadEvent BodyEntered;
			{
				auto Value = std::shared_ptr<int>(new int(42), [](int* P) { (void)GetTaskSchedulerDiagnostics(); delete P; });
				Weak = Value;
				auto Input = LaunchOutcome(Ready, std::move(Value));
				if (AlreadyCompleted) { Ready.Trigger(); EXPECT_TRUE(Input.TryRetractAndExecute()); }
				Dependent = Tasks::LaunchTask("DependencyLifetimeBody", [&] {
					EXPECT_TRUE(Weak.expired()); BodyEntered.Trigger();
				}, Tasks::Prerequisites(Input, Gate));
				if (!AlreadyCompleted) { Ready.Trigger(); EXPECT_TRUE(Input.TryRetractAndExecute()); }
			}
			EXPECT_EQ(Weak.expired(), AlreadyCompleted);
			Gate.Trigger();
			// Normal dispatch must release the dependent's pins before its body.
			Release.Trigger();
			ASSERT_TRUE(BodyEntered.WaitFor(2));
			EXPECT_EQ(Dependent.WaitDetailed().TaskState, ETaskState::Succeeded);
			Blocker.Wait();
		}
	}

	TEST(FTaskCompositionTests, NestedDependenciesRetainResultsUntilTheirParentCloses)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("NestedLifetimeGate", false), Ready("ChildReady", false);
		std::weak_ptr<int> Weak;
		Tasks::TTask<void> Parent;
		{
			auto Value = std::shared_ptr<int>(new int(42), [&Parent](int* P) {
				EXPECT_TRUE(Parent.IsCompleted());
				EXPECT_EQ(Parent.WaitDetailed(0).WaitStatus, ETaskWaitStatus::Completed);
				(void)GetTaskSchedulerDiagnostics(); delete P;
			});
			Weak = Value;
			auto Child = LaunchOutcome(Ready, std::move(Value));
			FThreadEvent Bound, ProbeRan;
			Parent = Tasks::LaunchTask("NestedLifetimeParent", [Child, &Gate, &Bound] {
				Tasks::AddNested(Child); Tasks::AddNested(Gate); Bound.Trigger();
			});
			ASSERT_TRUE(Bound.WaitFor(2));
			auto Probe = Tasks::LaunchTask("NestedLifetimeBodyRetired", [&] { ProbeRan.Trigger(); });
			ASSERT_TRUE(ProbeRan.WaitFor(2)); Probe.Wait();
			Ready.Trigger();
			ASSERT_EQ(Child.WaitDetailed().TaskState, ETaskState::Succeeded);
			FThreadEvent ChildRetired;
			auto RetirementProbe = Tasks::LaunchTask("NestedChildRetired", [&] { ChildRetired.Trigger(); });
			ASSERT_TRUE(ChildRetired.WaitFor(2)); RetirementProbe.Wait();
		}
		EXPECT_FALSE(Weak.expired());
		EXPECT_FALSE(Parent.IsCompleted());
		Gate.Trigger();
		EXPECT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(Weak.expired());
	}

	TEST(FTaskCompositionTests, EmptyPrerequisitesDoNotDelayTasksEventsOrDependents)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTask Empty;
		Tasks::TTask<int> EmptyTyped;
		Tasks::FTaskEvent Gate("NonemptyGate", false), Join("EmptyPrerequisiteJoin", false);
		Join.AddPrerequisites(Tasks::Prerequisites(Empty, Gate, EmptyTyped));
		Join.Trigger();
		std::atomic<uint32> Runs = 0;
		auto Root = Tasks::LaunchTask("MixedEmptyPrerequisites", [&] { ++Runs; return 42; },
			Tasks::Prerequisites(EmptyTyped, Join, Empty));
		const auto Extra = Tasks::Prerequisites(Empty, EmptyTyped);
		auto Next = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.Prerequisites = {Extra.begin(), Extra.end()}}, [&, Root]() {
			auto& Value = Root.GetResult(); ++Runs; return Value + 1; }, Root);
		EXPECT_EQ(Root.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_EQ(Runs.load(), 0u);
		EXPECT_EQ(Root.GetDiagnostics().PrerequisiteTaskIds, std::vector<uint64>{Join.GetTaskId()});
		Gate.Trigger();
		EXPECT_EQ(Next.GetResult(), 43);
		EXPECT_EQ(Runs.load(), 2u);
		Tasks::FTaskEvent EmptyJoin("OnlyEmptyPrerequisites", false);
		EmptyJoin.AddPrerequisites(Extra);
		EmptyJoin.Trigger();
		EXPECT_TRUE(EmptyJoin.IsCompleted());
		auto Independent = Tasks::LaunchIndependentTask("IndependentEmptyPrerequisites", [] { return 7; },
			{.Prerequisites = {Extra.begin(), Extra.end()}});
		EXPECT_EQ(Independent.GetResult(), 7);
	}

	TEST(FTaskCompositionTests, PublicGenericHandlesObserveTypedTasksAndEventsTogether)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("PublicHandleGate", false);
		auto Unique = Tasks::LaunchTask("PublicHandleUnique", [] { return 42; }, Gate);
		auto Shared = Tasks::LaunchTask("PublicHandleShared", [] { return 7; }, Gate);
		std::atomic<uint32> Runs = 0;
		auto Void = Tasks::LaunchTask("PublicHandleVoid", [&] { ++Runs; }, Gate);
		const std::array<Tasks::FTask, 4> Handles{Unique, Shared, Void, Gate};
		auto Copies = Handles;
		Tasks::FTask Signal = Gate;
		EXPECT_TRUE(Gate.IsValid());
		EXPECT_EQ(Signal.GetTaskId(), Copies.back().GetTaskId());
		EXPECT_FALSE(Signal.IsCompleted());
		auto Predecessor = Copies.front();
		auto Continuation = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [Predecessor](){ return 9; }, Predecessor);
		Gate.Trigger();
		for (const auto& Outcome : Tasks::WaitDetailed(Copies))
			EXPECT_EQ(Outcome.TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Runs.load(), 1u);
		EXPECT_EQ(Shared.GetResult(), 7);
		EXPECT_EQ(std::move(Unique.GetResult()), 42);
		EXPECT_TRUE(Unique.IsValid());
		EXPECT_EQ(Handles.front().WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(std::move(Continuation.GetResult()), 9);
	}

	TEST(FTaskCompositionTests, ExplicitCancellationCaptureDoesNotAffectIndependentDependents)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FCancellationToken Cancellation;
		FThreadEvent Started, Release;
		bool ObservedCancellation = false;
		auto Root = Tasks::LaunchTask("TokenRoot", [&]() {
			const auto& Token = Cancellation;
			Started.Trigger();
			Release.Wait();
			ObservedCancellation = Token.IsCanceled();
		}, {});
		const bool DidStart = Started.WaitFor(1.0);
		Cancellation.Cancel();
		Release.Trigger();
		ASSERT_TRUE(DidStart);
		ASSERT_EQ(Root.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(ObservedCancellation);
		Tasks::FCancellationToken IndependentCancellation;

		auto ValueInput = Tasks::LaunchTask("TokenValue", [] { return std::make_unique<int>(13); });
		auto Value = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [ValueInput, &IndependentCancellation]() {
			const auto& Token = IndependentCancellation;
				auto& Input = ValueInput.GetResult();
				EXPECT_FALSE(Token.IsCanceled());
				return *Input + 2;
			}, ValueInput);
		EXPECT_EQ(std::move(Value.GetResult()), 15);
		auto VoidInput = Tasks::LaunchTask("TokenVoid", [] {});
		auto Void = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [VoidInput, &IndependentCancellation]() {
			const auto& Token = IndependentCancellation; EXPECT_FALSE(Token.IsCanceled()); return 17; }, VoidInput);
		EXPECT_EQ(std::move(Void.GetResult()), 17);
		Tasks::TTask<int> Child;
		auto ParentInput = Tasks::LaunchTask("TokenNested", [] { return 19; });
		auto Parent = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&, ParentInput]() {
			const auto& Token = IndependentCancellation;
				auto& Input = ParentInput.GetResult();
				EXPECT_FALSE(Token.IsCanceled());
				Child = Tasks::LaunchTask("TokenChild", [Input] { return Input + 2; }, {});
				Tasks::AddNested(Child);
			}, ParentInput);
		ASSERT_EQ(WaitOffGameThread(Parent).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Child.GetResult(), 21);
	}

	TEST(FTaskCompositionTests, DirectLaunchAndAddNestedKeepParentPendingWithoutBlockingWorker)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .NumBlockingIOThreads = 1}));
		FThreadEvent WorkerFreed, ReleaseChild;
		Tasks::FTask ChildObservation;
		auto Parent = Tasks::LaunchTask("DirectNestedParent", [&]() {
			auto Child = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [&] { ReleaseChild.Wait(); });
			ChildObservation = Child;
			Tasks::AddNested(Child);
			auto Marker = Tasks::LaunchTask("AfterParentBody", [&] { WorkerFreed.Trigger(); });
			return 42;
		});
		const bool WorkerWasFreed = WorkerFreed.WaitFor(1.0);
		EXPECT_TRUE(WorkerWasFreed);
		if (WorkerWasFreed)
		{
			EXPECT_FALSE(Parent.IsCompleted());
			EXPECT_EQ(ChildObservation.GetDiagnostics().ParentTaskId, Parent.GetTaskId());
		}
		ReleaseChild.Trigger();
		ASSERT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(std::move(Parent.GetResult()), 42);
	}

	TEST(FTaskCompositionTests, CommonHandleObservationTracksMovesAndResultOwnership)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<uint32> Destroyed = 0;
		struct FValue
		{
			explicit FValue(std::atomic<uint32>& InDestroyed) : Counter(InDestroyed) {}
			~FValue() { ++Counter; }
			std::atomic<uint32>& Counter;
		};
		auto Original = Tasks::LaunchTask("BaseObservation", [&] { return std::make_unique<FValue>(Destroyed); });
		const Durin::Tasks::FTask Observer = Original;
		auto AssignedSource = Original;
		auto ContainerSource = Original;
		auto TypedSource = Original;
		const Durin::Tasks::FTask RvalueObserver(std::move(Original));
		EXPECT_FALSE(Original.IsValid());
		EXPECT_EQ(RvalueObserver.GetTaskId(), Observer.GetTaskId());
		Tasks::FTask AssignedObserver;
		AssignedObserver = std::move(AssignedSource);
		std::vector<Tasks::FTask> Observations;
		Observations.emplace_back(std::move(ContainerSource));
		EXPECT_FALSE(AssignedSource.IsValid());
		EXPECT_FALSE(ContainerSource.IsValid());
		EXPECT_EQ(AssignedObserver.GetTaskId(), Observer.GetTaskId());
		EXPECT_EQ(Observations.front().GetTaskId(), Observer.GetTaskId());
		auto MovedObserver = std::move(AssignedObserver);
		EXPECT_FALSE(AssignedObserver.IsValid());
		AssignedObserver = std::move(MovedObserver);
		EXPECT_FALSE(MovedObserver.IsValid());
		EXPECT_EQ(AssignedObserver.GetTaskId(), Observer.GetTaskId());
		const Durin::Tasks::FTask& TemporaryObserver = Tasks::LaunchTask("TemporaryObservation", [] {});
		ASSERT_TRUE(TemporaryObserver.IsValid());
		EXPECT_EQ(WaitTask(TemporaryObserver).TaskState, ETaskState::Succeeded);
		auto Moved = std::move(TypedSource);
		EXPECT_FALSE(TypedSource.IsValid());
		const Tasks::FTask& Base = Moved;
		ASSERT_EQ(Base.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Base.GetTaskId(), Observer.GetTaskId());
		EXPECT_TRUE(Base.IsCompleted());
		EXPECT_EQ(Destroyed, 0u);
		Moved = Tasks::LaunchTask("OverwriteResult", [&] { return std::make_unique<FValue>(Destroyed); });
		EXPECT_EQ(Destroyed, 0u); // Generic observers still retain the first result.
		ASSERT_EQ(Moved.WaitDetailed().TaskState, ETaskState::Succeeded);
		auto Shared = Moved;
		EXPECT_TRUE(Moved.IsValid());
		{
			// A const reference extends the lifetime of the erased value conversion.
			const Tasks::FTask& SharedBase = Shared;
			EXPECT_TRUE(SharedBase.IsCompleted());
			auto Alias = Shared.GetResultShared();
			ASSERT_TRUE(Alias);
			Shared = {};
			EXPECT_EQ(Destroyed, 0u);
			Moved = {};
			Alias.reset();
			ShutdownTaskScheduler(true); // Retire the executor's final node reference.
			EXPECT_EQ(Destroyed, 0u); // The erased value still owns the result.
		}
		EXPECT_EQ(Destroyed, 1u);
		EXPECT_TRUE(Observer.IsCompleted());
	}

	TEST(FTaskCompositionTests, SharedResultLivesUntilLastGenericHandleReleased)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<uint32> Destroyed = 0;
		struct FValue { std::atomic<uint32>& Count; ~FValue() { ++Count; } };
		Tasks::FTask Generic, Copy;
		{
			auto Typed = Tasks::LaunchTask("SharedResult", [&] { return std::make_unique<FValue>(Destroyed); });
			auto TypedCopy = Typed;
			ASSERT_EQ(Typed.WaitDetailed().TaskState, ETaskState::Succeeded);
			EXPECT_EQ(&Typed.GetResult(), &TypedCopy.GetResult());
			Generic = Typed;
			Copy = Generic;
		}
		ShutdownTaskScheduler(true); // Execution references retire; generic handles still own the result.
		EXPECT_EQ(Destroyed, 0u);
		EXPECT_EQ(Generic.WaitDetailed().TaskState, ETaskState::Succeeded);
		Generic = {};
		EXPECT_EQ(Destroyed, 0u);
		Copy = {};
		EXPECT_EQ(Destroyed, 1u);
	}

	TEST(FTaskCompositionTests, DependentResultViewPreservesMoveOnlyValueAndOwnerLifetime)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<uint32> Destroyed = 0;
		struct FValue { std::atomic<uint32>& Count; ~FValue() { ++Count; } };
		Tasks::FTask Generic;
		{
			auto Inner = Tasks::LaunchTask("SharedValue", [&] { return std::make_unique<FValue>(Destroyed); });
			auto View = Tasks::LaunchTask("ResultView", [Inner] { return Inner.GetResultShared(); }, Inner);
			ASSERT_EQ(WaitOffGameThread(View).TaskState, ETaskState::Succeeded);
			EXPECT_EQ(View.GetResult().get(), &Inner.GetResult());
			Generic = View;
		}
		ShutdownTaskScheduler(true);
		EXPECT_EQ(Destroyed, 0u);
		Generic = {};
		EXPECT_EQ(Destroyed, 1u);
	}

	TEST(FTaskCompositionTests, DirectMixedPrerequisitesPreserveTypedResults)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("MixedPrerequisites", false);
		auto Unique = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {},
			[] { return std::make_unique<int>(17); }, Gate);
		auto Shared = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {},
			[] { return 23; }, Gate);
		const auto Inputs = Tasks::Prerequisites(Unique, Shared, Gate);
		auto Dependent = Tasks::LaunchTask("MixedDependent", [] { return 31; }, Inputs);
		EXPECT_FALSE(Dependent.IsCompleted());
		Gate.Trigger();
		ASSERT_EQ(Tasks::WaitDetailed(Dependent).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Dependent.GetResult(), 31);
		EXPECT_EQ(*std::move(Unique.GetResult()), 17);
		EXPECT_EQ(Shared.GetResult(), 23);
	}

	TEST(FTaskCompositionTests, EventPrerequisitesReleaseOrdinaryDependents)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("EventContinuation", false);
		std::atomic<uint32> Runs = 0;
		auto Canceled = Tasks::LaunchTask("CanceledEventDependent", [&] { ++Runs; }, Gate);
		auto Continuation = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [Gate](){ return 41; }, Gate);

		EXPECT_FALSE(Gate.IsCompleted());
		Gate.Trigger();
		EXPECT_EQ(Tasks::WaitDetailed(Canceled).TaskState, ETaskState::Succeeded);
		ASSERT_EQ(Tasks::WaitDetailed(Continuation).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Continuation.GetResult(), 41);
		EXPECT_EQ(Runs, 1u);
	}

	TEST(FTaskCompositionTests, TimedWaitPreservesPendingResultsAndSharesCollectionDeadline)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("TimedWaitGate", false);
		auto Pending = Tasks::LaunchTask("TimedWaitValue", [] { return 42; }, Gate);
		EXPECT_EQ(Pending.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_EQ(Tasks::WaitDetailed(Pending, 0.002).WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_FALSE(Pending.IsCompleted());
		EXPECT_EQ(Pending.GetState(), ETaskState::Waiting);
		Gate.Trigger();
		ASSERT_EQ(Pending.WaitDetailed(1).WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_EQ(std::move(Pending.GetResult()), 42);
		EXPECT_EQ(Tasks::WaitDetailed(std::array<Tasks::TTask<void>, 0>{}, 0).size(), 0u);
		EXPECT_EQ(Tasks::WaitDetailed(Tasks::TTask<void>{}, 0).WaitStatus, ETaskWaitStatus::Completed);

		std::atomic<uint32> LateRuns = 0;
		FThreadEvent ParentBodyFinished;
		auto Parent = Tasks::LaunchTask("TimedCollectionParent", [&] {
			FThreadEvent NeverSignaled;
			std::vector<Tasks::TTask<void>> Inputs;
			Inputs.push_back(Tasks::LaunchTask("BudgetConsumingBody", [&] { NeverSignaled.WaitFor(0.06); }));
			Inputs.push_back(Tasks::LaunchTask("AfterCollectionDeadline", [&] { ++LateRuns; }));
			const auto Outcomes = Tasks::WaitDetailed(Inputs, 0.02);
			EXPECT_EQ(Outcomes[0].WaitStatus, ETaskWaitStatus::Completed);
			EXPECT_EQ(Outcomes[1].WaitStatus, ETaskWaitStatus::TimedOut);
			EXPECT_EQ(LateRuns.load(), 0u);
			// Even a failed timing assertion must retire captures before returning.
			Inputs[0].Wait();
			Tasks::AddNested(Inputs[1]);
			ParentBodyFinished.Trigger();
		});
		// Keep the caller from retracting the parent or racing its child on the
		// single worker: this case specifically tests inline timeout overrun.
		EXPECT_TRUE(ParentBodyFinished.WaitFor(5));
		EXPECT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(LateRuns.load(), 1u);
	}

	TEST(FTaskCompositionTests, WaitHelpsOnlyTargetDependenciesAndNestedWork)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<uint32> UnrelatedRuns = 0;
		FThreadEvent ParentStarted;
		auto Parent = Tasks::LaunchTask("DirectedWaitParent", [&] {
			ParentStarted.Trigger();
			auto Unrelated = Tasks::LaunchTask("UnrelatedHighPriority", [&] { ++UnrelatedRuns; },
				{.Priority = ETaskPriority::High});
			std::vector<Tasks::TTask<int>> Inputs;
			Inputs.push_back(Tasks::LaunchTask("DirectedPrerequisite", [] { return 3; }));
			auto Target = Tasks::LaunchTask("DirectedTarget", [] { return 7; }, Inputs);
			ASSERT_EQ(Target.WaitDetailed(1).TaskState, ETaskState::Succeeded);
			EXPECT_EQ(UnrelatedRuns.load(), 0u);
			EXPECT_EQ(std::move(Inputs.front().GetResult()), 3);
			EXPECT_EQ(std::move(Target.GetResult()), 7);
			auto Nested = Tasks::LaunchTask("DirectedNestedParent", [] {
				auto Child = Tasks::LaunchTask("DirectedNestedChild", [] {});
				Tasks::AddNested(Child);
			});
			ASSERT_EQ(Nested.WaitDetailed(1).TaskState, ETaskState::Succeeded);
			auto Immediate = Tasks::LaunchTask("ZeroTimeoutRetraction", [] { return 5; });
			EXPECT_EQ(Immediate.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
			EXPECT_TRUE(Immediate.TryRetractAndExecute());
			EXPECT_EQ(std::move(Immediate.GetResult()), 5);
			Tasks::AddNested(Unrelated);
		});
		ASSERT_TRUE(ParentStarted.WaitFor(2));
		EXPECT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(UnrelatedRuns.load(), 1u);
	}

	TEST(FTaskCompositionTests, RetractionHandlesDeepChainsSharedDagsAndNewNestedWork)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		std::atomic<uint32> Runs = 0, UnrelatedRuns = 0;
		FThreadEvent ParentStarted;
		auto Parent = Tasks::LaunchTask("RetractionGraphParent", [&] {
			ParentStarted.Trigger();
			auto Unrelated = Tasks::LaunchTask("UnrelatedDuringRetraction", [&] { ++UnrelatedRuns; }, {.Priority = ETaskPriority::High});
			std::vector<Tasks::FTask> Chain;
			Chain.push_back(Tasks::LaunchTask("DeepRoot", [&] { ++Runs; }));
			for (uint32 Index = 1; Index < 512; ++Index)
			{
				std::array<Tasks::FTask, 1> Previous{Chain.back()};
				Chain.push_back(Tasks::LaunchTask("DeepStep", [&] { ++Runs; }, Previous));
			}
			std::vector<Tasks::FTask> Branches;
			for (uint32 Index = 0; Index < 64; ++Index)
			{
				std::array<Tasks::FTask, 1> SharedLeaf{Chain.back()};
				Branches.push_back(Tasks::LaunchTask("SharedDagBranch", [&] { ++Runs; }, SharedLeaf));
			}
			auto Target = Tasks::LaunchTask("DagNestedTarget", [&] {
				auto Child = Tasks::LaunchTask("NewNestedChild", [&] {
					auto Grandchild = Tasks::LaunchTask("NewNestedGrandchild", [&] { ++Runs; });
					Tasks::AddNested(Grandchild);
				});
				Tasks::AddNested(Child);
			}, Branches);
			EXPECT_EQ(Target.WaitDetailed(3).TaskState, ETaskState::Succeeded);
			EXPECT_EQ(Runs.load(), 577u);
			EXPECT_EQ(UnrelatedRuns.load(), 0u);
			Tasks::AddNested(Unrelated);
		});
		ASSERT_TRUE(ParentStarted.WaitFor(2));
		EXPECT_EQ(Parent.WaitDetailed(5).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(UnrelatedRuns.load(), 1u);
	}

	TEST(FTaskCompositionTests, ConcurrentRetractionWaitersExecuteSharedDependenciesOnce)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(2));
		FThreadEvent FirstStarted, SecondStarted, Release;
		Tasks::FTask Target;
		std::atomic<uint32> Runs = 0;
		auto First = Tasks::LaunchTask("FirstRetractionWaiter", [&] {
			FirstStarted.Trigger(); Release.WaitFor(2);
			EXPECT_EQ(WaitTask(Target, 2).TaskState, ETaskState::Succeeded);
		});
		auto Second = Tasks::LaunchTask("SecondRetractionWaiter", [&] {
			SecondStarted.Trigger(); Release.WaitFor(2);
			EXPECT_EQ(WaitTask(Target, 2).TaskState, ETaskState::Succeeded);
		});
		EXPECT_TRUE(FirstStarted.WaitFor(1)); EXPECT_TRUE(SecondStarted.WaitFor(1));
		std::vector<Tasks::FTask> Inputs;
		for (uint32 Index = 0; Index < 64; ++Index)
			Inputs.push_back(Tasks::LaunchTask("SharedRetractionInput", [&] { ++Runs; }));
		auto Joined = Tasks::LaunchTask("SharedRetractionTarget", [&] { ++Runs; }, Inputs);
		Target = Joined;
		Release.Trigger();
		EXPECT_EQ(First.WaitDetailed(3).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Second.WaitDetailed(3).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Joined.WaitDetailed(1).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Runs.load(), 65u);
	}

	TEST(FTaskCompositionTests, ExternalRetractionExecutesOnlyTargetGraphWithoutWaiting)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started, Release;
		struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } Resume{Release};
		auto Blocker = Tasks::LaunchTask("RetractionBlocker", [&] { Started.Trigger(); Release.Wait(); });
		ASSERT_TRUE(Started.WaitFor(1));
		std::atomic<uint32> UnrelatedRuns = 0;
		auto Unrelated = Tasks::LaunchTask("RetractionUnrelated", [&] { ++UnrelatedRuns; }, {.Priority = ETaskPriority::High});
		auto Input = Tasks::LaunchTask("RetractionInput", [] { return 41; });
		const auto CallerId = FPlatformLTS::GetCurrentThreadId();
		auto Target = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [CallerId, Input]() {
			auto& Value = Input.GetResult();
			EXPECT_EQ(CallerId, FPlatformLTS::GetCurrentThreadId());
			return Value + 1;
		}, Input);
		EXPECT_EQ(Target.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_EQ(Target.WaitDetailed(-1).WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_EQ(Target.WaitDetailed(std::numeric_limits<double>::quiet_NaN()).WaitStatus, ETaskWaitStatus::TimedOut);
		const std::array<Tasks::FTask, 2> PollTargets{Input, Target};
		for (const auto& Outcome : Tasks::WaitDetailed(PollTargets, 0)) EXPECT_EQ(Outcome.WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_FALSE(Input.IsCompleted());
		EXPECT_FALSE(Blocker.TryRetractAndExecute());
		EXPECT_TRUE(Target.TryRetractAndExecute());
		EXPECT_EQ(Target.GetResult(), 42);
		EXPECT_EQ(UnrelatedRuns, 0u);
		Release.Trigger();
		EXPECT_EQ(Unrelated.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Blocker.WaitDetailed().TaskState, ETaskState::Succeeded);
	}

	TEST(FTaskCompositionTests, OrdinaryThreadWaitRetractsItsGraphAndRestoresTaskContext)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started, Release;
		struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } Resume{Release};
		auto Blocker = Tasks::LaunchTask("OrdinaryWaitBlocker", [&] { Started.Trigger(); Release.Wait(); });
		ASSERT_TRUE(Started.WaitFor(2));
		std::atomic<uint32> UnrelatedRuns = 0, NestedRuns = 0;
		auto Unrelated = Tasks::LaunchTask("OrdinaryWaitUnrelated", [&] { ++UnrelatedRuns; }, {.Priority = ETaskPriority::High});
		auto Input = Tasks::LaunchTask("OrdinaryWaitInput", [] { return 41; });
		uint32 CallerId = 0;
		auto Target = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&, Input]() {
			auto& Value = Input.GetResult();
			EXPECT_EQ(CallerId, FPlatformLTS::GetCurrentThreadId());
			auto Child = Tasks::LaunchTask("OrdinaryWaitNested", [&] {
				EXPECT_EQ(CallerId, FPlatformLTS::GetCurrentThreadId()); ++NestedRuns;
			});
			Tasks::AddNested(Child);
			return Value + 1;
		}, Input);
		Tasks::FTaskEvent Join("OrdinaryWaitEvent", false);
		Join.AddPrerequisites(Target); Join.Trigger();
		FTaskWaitResult Result;
		std::thread Caller([&] {
			CallerId = FPlatformLTS::GetCurrentThreadId();
			Result = Join.WaitDetailed(2);
			EXPECT_EQ(Private::FTaskRuntimeAccess::GetCurrentTaskId(), 0u);
		});
		Caller.join();
		const auto UnrelatedBeforeRelease = UnrelatedRuns.load();
		Release.Trigger();
		EXPECT_EQ(Result.TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Target.GetResult(), 42);
		EXPECT_EQ(NestedRuns.load(), 1u);
		EXPECT_EQ(UnrelatedBeforeRelease, 0u);
		Blocker.Wait(); Unrelated.Wait();
	}

	TEST(FTaskCompositionTests, OrdinaryWaitRetractsCpuPrerequisitesWithoutBlockingIoAuthority)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .NumBlockingIOThreads = 1}));
		FThreadEvent CpuStarted, IoStarted, Release;
		struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } Resume{Release};
		auto CpuBlocker = Tasks::LaunchTask("ExternalCpuBlocker", [&] { CpuStarted.Trigger(); Release.Wait(); });
		auto IoBlocker = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [&] { IoStarted.Trigger(); Release.Wait(); });
		ASSERT_TRUE(CpuStarted.WaitFor(2)); ASSERT_TRUE(IoStarted.WaitFor(2));
		std::atomic<uint32> CpuRuns = 0, IoRuns = 0;
		auto Cpu = Tasks::LaunchTask("ExternalCpuPrerequisite", [&] { ++CpuRuns; });
		const auto Dependencies = Tasks::Prerequisites(Cpu);
		auto Io = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {.Prerequisites = {Dependencies.begin(), Dependencies.end()}}, [&] { ++IoRuns; });
		FTaskWaitResult Result;
		std::thread Caller([&] { Result = Io.WaitDetailed(0.02); });
		Caller.join();
		const auto CpuBeforeRelease = CpuRuns.load(), IoBeforeRelease = IoRuns.load();
		Release.Trigger();
		EXPECT_EQ(Result.WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_EQ(CpuBeforeRelease, 1u); EXPECT_EQ(IoBeforeRelease, 0u);
		EXPECT_EQ(Io.WaitDetailed().TaskState, ETaskState::Succeeded);
		CpuBlocker.Wait(); IoBlocker.Wait();
	}

	TEST(FTaskCompositionTests, ZeroTimeoutOnlyPollsAndExplicitRetractionExecutesNestedWork)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent ParentStarted;
		auto Parent = Tasks::LaunchTask("ZeroNestedWaiter", [&] {
			ParentStarted.Trigger();
			uint32 ChildRuns = 0;
			bool BodyRan = false;
			auto Target = Tasks::LaunchTask("ZeroNestedTarget", [&] {
				BodyRan = true;
				auto Child = Tasks::LaunchTask("ZeroNestedChild", [&] { ++ChildRuns; });
				Tasks::AddNested(Child);
			});
			EXPECT_EQ(Target.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
			EXPECT_FALSE(BodyRan);
			EXPECT_EQ(ChildRuns, 0u);
			EXPECT_TRUE(Target.TryRetractAndExecute());
			EXPECT_TRUE(BodyRan);
			EXPECT_EQ(ChildRuns, 1u);
		});
		ASSERT_TRUE(ParentStarted.WaitFor(2));
		EXPECT_EQ(Parent.WaitDetailed(3).TaskState, ETaskState::Succeeded);
	}

	TEST(FTaskCompositionTests, NotifiedWaitResumesForLateEventPrerequisite)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		for (uint32 Iteration = 0; Iteration < 32; ++Iteration)
		{
			Tasks::FTaskEvent Result("LatePrerequisiteTarget", false);
			FThreadEvent Ready;
			auto Waiter = Tasks::LaunchTask("LatePrerequisiteWaiter", [&] {
				EXPECT_EQ(Result.WaitDetailed(0.001).WaitStatus, ETaskWaitStatus::TimedOut);
				Ready.Trigger();
				EXPECT_EQ(Result.WaitDetailed(2).TaskState, ETaskState::Succeeded);
			});
			EXPECT_TRUE(Ready.WaitFor(1));
			auto Child = Tasks::LaunchTask("LatePrerequisiteChild", [] { return 17; });
			Result.AddPrerequisites(Child);
			Result.Trigger();
			EXPECT_EQ(Waiter.WaitDetailed(3).TaskState, ETaskState::Succeeded);
			EXPECT_EQ(Child.GetResult(), 17);
		}
	}

	TEST(FTaskCompositionTests, NotifiedWaitRevisitsRunningIoTaskAfterNestedRegistration)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .NumBlockingIOThreads = 1}));
		FThreadEvent Waiting, AllowNested;
		std::atomic<uint32> ChildRuns = 0;
		auto Parent = Tasks::LaunchTask("CrossExecutorWaiter", [&] {
			auto Io = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [&] {
				AllowNested.WaitFor(2);
				auto Child = Tasks::LaunchTask("LateCpuNestedChild", [&] { ++ChildRuns; });
				Tasks::AddNested(Child);
			});
			EXPECT_EQ(Io.WaitDetailed(0.003).WaitStatus, ETaskWaitStatus::TimedOut);
			Waiting.Trigger();
			EXPECT_EQ(Io.WaitDetailed(2).TaskState, ETaskState::Succeeded);
		});
		EXPECT_TRUE(Waiting.WaitFor(1));
		AllowNested.Trigger();
		EXPECT_EQ(Parent.WaitDetailed(3).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(ChildRuns.load(), 1u);
	}

	TEST(FTaskCompositionTests, TimedWaitIncludesTerminalPublicationAndKeepsOwnerThreadPolicy)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Ready("TerminalHookReady", false);
		auto Result = LaunchOutcome(Ready, 23);
		FThreadEvent HookEntered, ReleaseHook;
		auto Hook = std::make_shared<Private::FTaskBeforeCompletionHook>();
		Hook->Function = [&](ETaskState) { HookEntered.Trigger(); ReleaseHook.Wait(); };
		Private::FTaskRuntimeAccess::BindBeforeCompletion(Result, Hook);
		Ready.Trigger();
		EXPECT_TRUE(HookEntered.WaitFor(1));
		FTaskWaitResult TimedResult;
		std::thread Waiter([&] { TimedResult = Result.WaitDetailed(0.002); });
		Waiter.join();
		EXPECT_EQ(TimedResult.WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_FALSE(Result.IsCompleted());
		ReleaseHook.Trigger();
		ASSERT_EQ(WaitOffGameThread(Result).TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Result.WaitDetailed(0).WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_EQ(std::move(Result.GetResult()), 23);
		Tasks::FTaskEvent Unknown("UnknownTimedWait");
		EXPECT_EQ(Unknown.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
		EXPECT_FALSE(Unknown.TryRetractAndExecute());
		Unknown.Trigger();
		EXPECT_EQ(Unknown.WaitDetailed(0).WaitStatus, ETaskWaitStatus::Completed);
	}

	TEST(FTaskCompositionTests, DirectedHelpingRacesWorkersWithoutDuplicateExecution)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(2));
		std::array<std::atomic<uint32>, 128> Runs{};
		auto Parent = Tasks::LaunchTask("RetractionRaceParent", [&] {
			std::vector<Tasks::TTask<void>> Children;
			for (size_t Index = 0; Index < Runs.size(); ++Index)
				Children.push_back(Tasks::LaunchTask("RetractionRaceChild", [&, Index] { ++Runs[Index]; }));
			for (const auto& Outcome : Tasks::WaitDetailed(Children, 2))
				EXPECT_EQ(Outcome.WaitStatus, ETaskWaitStatus::Completed);
		});
		ASSERT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Succeeded);
		for (const auto& Count : Runs) EXPECT_EQ(Count.load(), 1u);
		ShutdownTaskScheduler(true);
	}

	TEST(FTaskCompositionTests, DependencyCollectionsPreserveResultsAndBindBeforeContainersExpire)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("CollectionGate", false), Extra("ExtraGate", false), Join("CollectionJoin", false);
		Tasks::FTaskExecutionOptions Options;
		const auto Existing = Tasks::Prerequisites(Extra);
		Options.Prerequisites.assign(Existing.begin(), Existing.end());
		std::vector<Tasks::TTask<int>> Unique;
		Unique.push_back(Tasks::LaunchTask("CollectionValue", [] { return 42; },
			std::vector<Tasks::FTaskEvent>{Gate}, Options));
		Join.AddPrerequisites(std::span(Unique));
		Join.Trigger();
		auto Dependent = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [] { return 7; }, std::move(Unique));
		Gate.Trigger();
		EXPECT_FALSE(Unique.front().IsCompleted());
		EXPECT_FALSE(Join.IsCompleted());
		Extra.Trigger();
		ASSERT_EQ(Tasks::WaitDetailed(std::as_const(Unique)).front().TaskState, ETaskState::Succeeded);
		ASSERT_EQ(Join.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(std::move(Dependent.GetResult()), 7);
		EXPECT_EQ(std::move(Unique.front().GetResult()), 42);

		const std::array Shared{Tasks::LaunchTask("SharedCollectionValue", [] { return 13; })};
		Tasks::FTaskEvent SharedJoin("SharedCollectionJoin", false);
		SharedJoin.AddPrerequisites(Shared);
		SharedJoin.Trigger();
		auto AfterShared = Tasks::LaunchTask("SharedCollectionDependent", [] {}, std::span(Shared));
		ASSERT_EQ(Tasks::WaitDetailed(Shared).front().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Shared.front().GetResult(), 13);
		EXPECT_EQ(AfterShared.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(SharedJoin.WaitDetailed().TaskState, ETaskState::Succeeded);
	}

	TEST(FTaskCompositionTests, CollectionWaitKeepsOutcomeOrderAndAcceptsEmptyInputs)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		std::array Events{Tasks::FTaskEvent("SucceededCollectionEvent", false),
			Tasks::FTaskEvent("CanceledCollectionEvent", false)};

		for (auto& Event : Events) Event.Trigger();
		const auto Outcomes = Tasks::WaitDetailed(std::span(Events));
		ASSERT_EQ(Outcomes.size(), 2u);
		EXPECT_EQ(Outcomes[0].TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Outcomes[1].TaskState, ETaskState::Succeeded);
		for (const auto& Outcome : Outcomes) EXPECT_EQ(Outcome.WaitStatus, ETaskWaitStatus::Completed);
		std::vector<Tasks::TTask<void>> Inputs;
		Inputs.push_back(Tasks::LaunchTask("FailedCollectionValue", [] { throw std::runtime_error("failure"); }));
		Inputs.emplace_back();
		const auto FailedOutcomes = Tasks::WaitDetailed(std::span(Inputs));
		ASSERT_EQ(FailedOutcomes.size(), 2u);
		EXPECT_EQ(FailedOutcomes[0].TaskState, ETaskState::Failed);
		EXPECT_EQ(FailedOutcomes[1].WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_EQ(FailedOutcomes[1].TaskState, ETaskState::Invalid);
		Inputs.clear();
		EXPECT_TRUE(Tasks::WaitDetailed(Inputs).empty());
		const std::array<Tasks::FTaskEvent, 0> Empty;
		Tasks::FTaskEvent EmptyJoin("EmptyCollectionJoin", false);
		EmptyJoin.AddPrerequisites(Inputs);
		EmptyJoin.AddPrerequisites(Empty);
		EmptyJoin.AddPrerequisites(std::span(Empty));
		EmptyJoin.Trigger();
		EXPECT_EQ(EmptyJoin.WaitDetailed().TaskState, ETaskState::Succeeded);
		auto EmptyLaunch = Tasks::LaunchTask("EmptyCollectionLaunch", [] {}, Empty);
		EXPECT_EQ(EmptyLaunch.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(Tasks::WaitDetailed(std::span(Empty)).empty());
	}

	TEST(FTaskCompositionTests, NestedCompletionReleasesWorkerAndDefersResultPublication)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("NestedGate");
		auto Parent = Tasks::LaunchTask("NestedParent", [&] {
			auto Child = Tasks::LaunchTask("NestedChild", [] { return 21; }, Gate);
			Tasks::AddNested(Child);
			Tasks::AddNested(Gate);
			return 42;
		});
		// On one worker this can finish only after the parent body has returned.
		FThreadEvent ProbeRan;
		auto Probe = Tasks::LaunchTask("FreeWorkerProbe", [&] { ProbeRan.Trigger(); });
		ASSERT_TRUE(ProbeRan.WaitFor(2));
		EXPECT_EQ(Probe.WaitDetailed().WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_FALSE(Parent.IsCompleted());
		EXPECT_EQ(GetTaskSchedulerDiagnostics().ExecutorStorage[0].CurrentRunningBodies, 0u);
		Gate.Trigger();
		EXPECT_EQ(std::move(Parent.GetResult()), 42);
	}

	TEST(FTaskCompositionTests, TaskEventJoinsCompletionWithoutPropagatingFailureOrCancellation)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent First("FirstSignal"), Second("SecondSignal"), Join("JoinedSignal");
		auto Failed = Tasks::LaunchTask("FailedPrerequisite", [] { throw std::runtime_error("expected child failure"); });
		EXPECT_EQ(Failed.WaitDetailed().TaskState, ETaskState::Failed);
		Join.AddPrerequisites(Failed);
		const std::array Inputs{First, Second};
		Join.AddPrerequisites(Inputs);
		Join.AddPrerequisites(std::span(Inputs));
		Join.Trigger();
		EXPECT_FALSE(Join.IsCompleted());
		First.Trigger();
		EXPECT_FALSE(Join.IsCompleted());

		EXPECT_FALSE(Second.IsCompleted());
		Second.Trigger();
		EXPECT_EQ(Second.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Join.WaitDetailed().TaskState, ETaskState::Succeeded);
	}

	TEST(FTaskCompositionTests, NestedParentFailureStillDrainsChildren)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Signal("FailureSignal");
		auto Parent = Tasks::LaunchTask("FailedNestedParent", [&] {
			Tasks::AddNested(Signal);
			throw std::runtime_error("parent failure");
		});
		FThreadEvent ProbeRan;
		auto Probe = Tasks::LaunchTask("FailureBodyReturned", [&] { ProbeRan.Trigger(); });
		ASSERT_TRUE(ProbeRan.WaitFor(2));
		EXPECT_EQ(Probe.WaitDetailed().WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_FALSE(Parent.IsCompleted());

		Signal.Trigger();
		EXPECT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Failed);
	}

	TEST(FTaskCompositionTests, NestedDependenciesPreserveGameThreadWaitPolicy)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		FThreadEvent Started, Release;
		struct FRelease { FThreadEvent& Event; ~FRelease() { Event.Trigger(); } } Resume{Release};
		auto Blocker = Tasks::LaunchTask("DeferredNestedBlocker", [&] { Started.Trigger(); Release.Wait(); });
		ASSERT_TRUE(Started.WaitFor(2));
		auto Parent = Tasks::LaunchTask("DeferredNestedParent", [] {
			auto Child = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred, {}, [] {});
			Tasks::AddNested(Child);
		});
		EXPECT_EQ(Parent.WaitDetailed().WaitStatus, ETaskWaitStatus::UnsupportedThread);
		PumpGameThreadDeferredWork();
		EXPECT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Succeeded);
		Release.Trigger();
		Blocker.Wait();
	}

	TEST(FTaskCompositionTests, CompletionDependencyCyclesAndInvalidNestedContextAreRejected)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent First("CycleFirst"), Second("CycleSecond");
		First.AddPrerequisites(Second);
		EXPECT_DEATH({ Second.AddPrerequisites(First); }, "");
		EXPECT_DEATH({ Tasks::AddNested(First); }, "");
		First.Trigger();
		EXPECT_FALSE(First.IsCompleted());
		Second.Trigger();
		EXPECT_EQ(First.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(Second.IsCompleted());
	}

	TEST(FTaskCompositionTests, NestedEventCompletionRacesBodyReturn)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(2));
		for (int Index = 0; Index != 64; ++Index)
		{
			Tasks::FTaskEvent Signal("RacingSignal");
			FThreadEvent Bound;
			auto Parent = Tasks::LaunchTask("RacingNestedParent", [&] { Tasks::AddNested(Signal); Bound.Trigger(); return 42; });
			const bool Registered = Bound.WaitFor(2.0);
			EXPECT_TRUE(Registered);
			Signal.Trigger();
			EXPECT_EQ(std::move(Parent.GetResult()), 42);
		}
	}

	TEST(FTaskCompositionTests, EventMovesTransferHandlesAndConcurrentTriggersRemainOneShot)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("TriggerGate", false), Event("IdempotentEvent", false);
		Event.AddPrerequisites(Gate);
		auto Signaler = Event;
		Tasks::FTask Observer = std::move(Event);
		EXPECT_FALSE(Event.IsValid());
		Event.Trigger(); // A moved-from event is an already-completed empty handle.
		std::atomic<uint32> Runs = 0;
		auto Dependent = Tasks::LaunchTask("TriggeredOnce", [&] { ++Runs; }, Observer);
		std::vector<std::thread> Callers;
		for (uint32 Index = 0; Index < 4; ++Index) Callers.emplace_back([Signaler]() mutable { Signaler.Trigger(); });
		for (auto& Caller : Callers) Caller.join();
		EXPECT_FALSE(Observer.IsCompleted());
		Gate.Trigger();
		EXPECT_EQ(Dependent.WaitDetailed().TaskState, ETaskState::Succeeded);
		Signaler.Trigger();
		EXPECT_EQ(Runs, 1u);
		EXPECT_TRUE(Observer.IsCompleted());
	}

	TEST(FTaskCompositionTests, EventsSurviveSchedulerStartupDrainAndRestart)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		Tasks::FTaskEvent Event("AcrossSchedulerLifetimes", false);
		const auto Id = Event.GetTaskId();
		EXPECT_EQ(Event.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_EQ(GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount, 0u);
		ShutdownTaskSystem(ETaskShutdownMode::Drain); // An untriggered event alone cannot hold Drain.
		EXPECT_FALSE(Event.IsCompleted());
		ASSERT_TRUE(InitializeTaskScheduler(1));

		ShutdownTaskSystem(ETaskShutdownMode::Drain);

		EXPECT_FALSE(Event.IsCompleted());
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Next = Tasks::LaunchTask("RestartedEventWaiter", [] { return 42; }, Event);
		Event.Trigger();
		EXPECT_EQ(Next.GetResult(), 42);
		ShutdownTaskScheduler(false);
		EXPECT_EQ(Event.GetTaskId(), Id);
		EXPECT_EQ(Event.WaitDetailed().TaskState, ETaskState::Succeeded);
		Tasks::FTaskEvent AfterShutdown("AfterShutdown", false);
		AfterShutdown.AddPrerequisites(Event);
		AfterShutdown.Trigger();
		EXPECT_TRUE(AfterShutdown.IsCompleted());
	}

	TEST(FTaskCompositionTests, TriggeredEventsRetainPendingCompletionWithoutFacadeOrScheduler)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		Tasks::FTaskEvent Gate("StandaloneGate", false), Join("StandaloneJoin", false);
		{
			Tasks::FTaskEvent Intermediate("StandaloneIntermediate", false);
			Intermediate.AddPrerequisites(Gate);
			Join.AddPrerequisites(Intermediate);
			Intermediate.Trigger();
		}
		Join.Trigger();
		EXPECT_FALSE(Join.IsCompleted());
		Gate.Trigger();
		EXPECT_EQ(Join.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(GetTaskSchedulerDiagnostics().RetainedTerminalHandleCount, 0u);
	}

	TEST(FTaskCompositionTests, EventWaitRetractsPrerequisitesOnTheOnlyWorker)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Parent = Tasks::LaunchTask("EventHelpingParent", [] {
			Tasks::FTaskEvent Event("EventHelpingJoin", false);
			auto Child = Tasks::LaunchTask("EventHelpingChild", [] { return 42; });
			Event.AddPrerequisites(Child);
			Event.Trigger();
			EXPECT_EQ(Event.WaitDetailed(2.0).TaskState, ETaskState::Succeeded);
			return Child.GetResult();
		});
		EXPECT_EQ(Parent.GetResult(), 42);
		Tasks::FTaskEvent External("ExternalEventWait", false);
		FThreadEvent Waiting;
		auto Waiter = Tasks::LaunchTask("EventExternalWaiter", [&] {
			Waiting.Trigger();
			return External.WaitDetailed(2.0);
		});
		const bool Started = Waiting.WaitFor(2.0);
		External.Trigger();
		EXPECT_TRUE(Started);
		EXPECT_EQ(Waiter.GetResult().TaskState, ETaskState::Succeeded);
	}

	TEST(FTaskCompositionTests, EventRetractionExecutesScheduledPrerequisites)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started, Release;
		auto Blocker = Tasks::LaunchTask("EventRetractionBlocker", [&] { Started.Trigger(); Release.Wait(); });
		const bool Running = Started.WaitFor(2.0);
		Tasks::FTaskEvent Event("RetractableEvent", false);
		auto Child = Tasks::LaunchTask("RetractableEventChild", [] { return 42; });
		Event.AddPrerequisites(Child);
		Event.Trigger();
		const bool Retracted = Event.TryRetractAndExecute();
		Release.Trigger();
		EXPECT_TRUE(Running);
		EXPECT_TRUE(Retracted);
		EXPECT_EQ(Child.GetResult(), 42);
		EXPECT_EQ(Blocker.WaitDetailed().TaskState, ETaskState::Succeeded);
	}

	TEST(FTaskCompositionTests, EventLifecycleChecksFollowTheNodeAcrossErasedHandles)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		Tasks::FTask Observer;
		{
			Tasks::FTaskEvent Event("RetainedUntriggeredNode", false);
			Observer = Event;
		}
		EXPECT_FALSE(Observer.IsCompleted());
		// Exercise the node directly after the last event facade has been released.
		Private::FTaskRuntimeAccess::TriggerTaskEvent(Observer);
		EXPECT_TRUE(Observer.IsCompleted());
		Observer = {};
		EXPECT_DEATH({
			Tasks::FTask Erased;
			{ Tasks::FTaskEvent Event("UncompletedErasedNode"); Erased = Event; }
			Erased = {};
		}, "Task event nodes must be completed");
	}

	TEST(FTaskCompositionTests, EventCompletionObserversCanRetriggerAndReleaseTheirSignalingHandle)
	{
		Tasks::FTaskEvent Event("ReentrantEvent", false);
		Tasks::FTask Observer = Event;
		uint32 Calls = 0;
		auto Subscription = Tasks::SubscribeTask(Event, [&](ETaskState) {
			EXPECT_TRUE(Event.IsCompleted());
			EXPECT_TRUE(Event.Wait());
			++Calls;
			Event.Trigger();
			Tasks::FTask Released = std::move(Event);
			Observer = {};
		});
		Event.Trigger();
		EXPECT_EQ(Calls, 1u);
		EXPECT_FALSE(Event.IsValid());
		EXPECT_FALSE(Observer.IsValid());
	}

	TEST(FTaskCompositionTests, TaskEventEnforcesSharedOneShotSignalAndNodeLifetime)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Signal("SharedSignal");
		{
			auto Copy = Signal;
			EXPECT_EQ(Copy.GetTaskId(), Signal.GetTaskId());
		}
		EXPECT_DEATH({ Tasks::AddNested(Signal); }, "Task construction failed");
		Signal.Trigger();
		EXPECT_TRUE(Signal.IsCompleted());
		EXPECT_DEATH({ Signal.AddPrerequisites(Signal); }, "before Trigger");
		EXPECT_DEATH({ Tasks::FTaskEvent Unsignaled("Unsignaled"); }, "Task event nodes must be completed");
	}

	TEST(FTaskCompositionTests, TrackedProducerSignalsEventDuringDrain)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		{
			ASSERT_TRUE(InitializeTaskScheduler(1));
			ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
			Tasks::FTaskEvent Signal("ShutdownSignal");
			int Calls = 0;
			std::atomic<bool> Retired = false;
			auto Handoff = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred, {}, [&] { ++Calls; }, Signal);
			struct FOwner { std::atomic<bool>& Flag; ~FOwner() { Flag = true; } };
			auto Producer = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {},
				[Signal, Owner = std::shared_ptr<FOwner>(new FOwner{Retired})]() mutable {
					while (IsTaskSchedulerRunning()) std::this_thread::yield();
					Signal.Trigger();
				});
			Producer = {}; // Scheduler drain must retain the actual producer, not just its signal.
			ShutdownTaskSystem(ETaskShutdownMode::Drain);
			EXPECT_TRUE(Retired.load());
			EXPECT_EQ(Calls, 1);
			EXPECT_EQ(Handoff.GetState(), ETaskState::Succeeded);
		}
	}

	TEST(FTaskCompositionTests, ShutdownAllowsCompletionFromDeferredCaptureDestruction)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTaskEvent Source("CaptureRetirementSignal");
		auto Handoff = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred, {}, [Source](){}, Source);
		int ReleasedCaptures = 0;
		auto Capture = std::shared_ptr<int>(new int(0), [Source, &ReleasedCaptures](int* Value) mutable {
			delete Value;
			++ReleasedCaptures;
			// Cancellation releases the queued callable before its task finishes
			// publishing. Completing another accepted task may dispatch a continuation.
			Source.Trigger();
		});
		auto Queued = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred, {},
			[Capture = std::move(Capture)] {
				(void)Capture;

			});

		ShutdownTaskSystem(ETaskShutdownMode::Drain);

		EXPECT_EQ(ReleasedCaptures, 1);
		EXPECT_EQ(Queued.GetState(), ETaskState::Succeeded);
		EXPECT_EQ(Handoff.GetState(), ETaskState::Succeeded);
		EXPECT_EQ(GetGameThreadDeferredWorkQueueDiagnostics().QueueDepth, 0u);
		EXPECT_FALSE(GetGameThreadDeferredWorkQueueDiagnostics().bInstalled);
		EXPECT_EQ(GetGameThreadDeferredWorkQueueDiagnostics().RejectedCount, 0u);
		EXPECT_EQ(GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount, 0u);
		EXPECT_EQ(GetTaskSchedulerDiagnostics().RejectedTaskCount, 0u);
	}

	TEST(FTaskCompositionTests, ResultAccessWaitsAndExplicitMovesPreserveHandles)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::TTask<int> Invalid;
		EXPECT_FALSE(Invalid.IsValid());
		EXPECT_TRUE(Invalid.IsCompleted());
		EXPECT_EQ(ETaskWaitStatus::Completed, Invalid.WaitDetailed().WaitStatus);
		EXPECT_TRUE(Invalid.TryRetractAndExecute());
		Tasks::FTaskExecutionOptions Options;
		auto Task = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, Options,
			[] { return std::make_unique<int>(42); });
		EXPECT_EQ(42, *Task.GetResult());
		EXPECT_TRUE(Task.IsCompleted());
		EXPECT_EQ(ETaskState::Succeeded, Task.GetState());
		auto Value = std::move(Task.GetResult());
		EXPECT_EQ(42, *Value);
		EXPECT_TRUE(Task.IsValid());
		EXPECT_FALSE(Task.GetResult()); // Explicit movement leaves a shared moved-from value.
		auto VoidTask = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [] {});
		EXPECT_EQ(VoidTask.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(VoidTask.IsValid());
		auto EmptyValue = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [] { return std::monostate{}; });
		EXPECT_EQ(std::monostate{}, EmptyValue.GetResult());
	}

	TEST(FTaskCompositionTests, LaunchTaskRejectsInvalidExecutor)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_DEATH({ (void)Tasks::LaunchTask(static_cast<Tasks::ETaskExecutor>(255), {}, [] {}); }, "Task construction failed");
	}

	TEST(FTaskCompositionTests, OrdinaryDependentsPreserveInputOutcomesMoveOnlyCapturesAndFanOut)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		for (auto Outcome : {ETaskState::Succeeded, ETaskState::Failed, ETaskState::Canceled})
		{
			SCOPED_TRACE(static_cast<int>(Outcome));
			Tasks::FTaskEvent Gate("DependentOutcome", false);
			auto Input = LaunchOutcome(Gate, std::make_unique<int>(7), Outcome);
			auto MakeDependent = [Input, Outcome] {
				return [Capture = std::make_unique<int>(5), Input, Outcome] {
					const auto& Task = Input;
					EXPECT_EQ(Outcome, Task.GetState());
					if (Outcome == ETaskState::Succeeded) return *Task.GetResult() + *Capture;
					if (Outcome == ETaskState::Failed)
					{
						EXPECT_EQ(Task.GetDiagnostics().TerminalReason, ETaskTerminalReason::CallbackFailure);
						EXPECT_NE(Task.GetDiagnostic().find("expected task failure"), std::string::npos);
					}
					return *Capture;
				};
			};
			auto First = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, MakeDependent(), Input);
			auto Second = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, MakeDependent(), Input);

			EXPECT_FALSE(Second.IsCompleted());
			Gate.Trigger();
			ASSERT_EQ(Tasks::WaitDetailed(First).TaskState, ETaskState::Succeeded);
			ASSERT_EQ(Tasks::WaitDetailed(Second).TaskState, ETaskState::Succeeded);
			const int Expected = Outcome == ETaskState::Succeeded ? 12 : 5;
			EXPECT_EQ(First.GetResult(), Expected);
			EXPECT_EQ(Second.GetResult(), Expected);
			EXPECT_TRUE(Input.IsValid());
			EXPECT_EQ(Input.GetState(), Outcome);
		}
	}

	TEST(FTaskCompositionTests, CancellationWaitsForTrackedProducerAndRemainsAdvisory)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FCancellationToken Cancellation;
		Tasks::FTaskEvent Gate("ExternalProgressGate");
		bool Observed = false;
		auto Result = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {},
			[&]() {
			const auto& Token = Cancellation; Observed = Token.IsCanceled(); return 42; }, Gate);
		EXPECT_EQ(Result.WaitDetailed().WaitStatus, ETaskWaitStatus::UnsupportedThread);
		Cancellation.Cancel();
		EXPECT_FALSE(Result.IsCompleted());
		Gate.Trigger();
		ASSERT_EQ(Result.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(Observed);
		EXPECT_EQ(Result.GetResult(), 42);
	}

	TEST(FTaskCompositionTests, OrdinarySubmissionDeduplicatesDependencies)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Primary("Primary", false), Extra("Extra", false);
		const std::array<Tasks::FTask, 5> Dependencies{Primary, {}, Extra, Primary, Extra};
		std::atomic<uint32> Calls = 0;
		auto Task = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.Prerequisites = {Dependencies.begin(), Dependencies.end()}}, [&] { ++Calls; });
		const auto Diagnostic = Task.GetDiagnostics();
		ASSERT_EQ(Diagnostic.PrerequisiteTaskIds.size(), 2u);
		EXPECT_EQ(Diagnostic.PrerequisiteTaskIds[0], Primary.GetTaskId());
		EXPECT_EQ(Diagnostic.PrerequisiteTaskIds[1], Extra.GetTaskId());
		Primary.Trigger();
		EXPECT_EQ(Task.WaitDetailed(0.0).WaitStatus, ETaskWaitStatus::TimedOut);
		Extra.Trigger();
		EXPECT_EQ(Task.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Calls.load(), 1u);
	}

	TEST(FTaskCompositionTests, SharedHandlesRetainNativeFailureAfterOriginalRelease)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Failed = Tasks::LaunchTask("ThrowBeforeShare", []() -> int { throw std::runtime_error("expected failure"); });
		ASSERT_EQ(Failed.WaitDetailed().TaskState, ETaskState::Failed);
		const auto NativeReason = Failed.GetDiagnostics().TerminalReason;
		const auto Diagnostic = Failed.GetDiagnostic();
		ASSERT_EQ(NativeReason, ETaskTerminalReason::CallbackFailure);
		EXPECT_NE(Diagnostic.find("expected failure"), std::string::npos);
		auto Shared = Failed;
		Failed = {};
		EXPECT_EQ(Shared.GetDiagnostics().TerminalReason, NativeReason);
		EXPECT_EQ(Shared.GetDiagnostic(), Diagnostic);
	}

	TEST(FTaskCompositionTests, NestedCompletionDoesNotForwardChildFailureOrCancellation)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (bool Canceled : {false, true})
		{
			Tasks::FTaskEvent Gate("NestedOutcome", false);
			auto Child = Tasks::LaunchTask("NestedOutcomeChild", [Canceled] {
				if (Canceled) Private::FTaskRuntimeAccess::SetCurrentCanceledOutcome();
				else throw std::runtime_error("child failure");
			}, Gate);
			FThreadEvent Attached;
			auto Parent = Tasks::LaunchTask("NestedOutcomeParent", [&] {
				Tasks::AddNested(Child);
				Attached.Trigger();
				return 42;
			});
			const bool DidAttach = Attached.WaitFor(1.0);
			EXPECT_FALSE(Parent.IsCompleted());
			Gate.Trigger();
			ASSERT_TRUE(DidAttach);
			EXPECT_EQ(WaitOffGameThread(Parent).TaskState, ETaskState::Succeeded);
			EXPECT_EQ(Parent.GetResult(), 42);
			EXPECT_EQ(Child.GetState(), Canceled ? ETaskState::Canceled : ETaskState::Failed);
		}
	}

	TEST(FTaskCompositionTests, NestedRunnerBindsBorrowedPrerequisitesBeforeReturning)
	{
		EnsureGameThreadForTaskTest(); ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard; ASSERT_TRUE(InitializeTaskScheduler(1));
		auto Check = []<typename T>(const Tasks::TTask<T>& Shared) {
			for (const bool bDestroyCollection : {false, true})
			{
				Tasks::FTaskEvent RootSource("BorrowedPrerequisiteRoot", false);
				Tasks::FTaskEvent Gate("BorrowedPrerequisiteGate", false);
				std::vector<Tasks::FTask> Borrowed{Gate};
				std::atomic<bool> bFactoryRan = false;
				auto Output = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.DebugName = "BorrowedPrerequisiteNested", .Prerequisites = Borrowed}, [&, RootSource](){
						Tasks::AddNested(Shared); bFactoryRan.store(true);
					}, RootSource);
				// Mutation catches a second binding deterministically; destruction also
				// exercises the lifetime of the borrowed storage without changing the API.
				if (bDestroyCollection) std::vector<Tasks::FTask>().swap(Borrowed);
				else Borrowed.front() = {};
				RootSource.Trigger();
				auto Probe = Tasks::LaunchTask("BorrowedPrerequisiteProbe", [] {});
				EXPECT_EQ(Probe.WaitDetailed(1).WaitStatus, ETaskWaitStatus::Completed);
				const bool bRanBeforePrerequisite = bFactoryRan.load();
				Gate.Trigger();
				EXPECT_FALSE(bRanBeforePrerequisite);
				ASSERT_EQ(WaitOffGameThread(Output).TaskState, ETaskState::Succeeded);
				EXPECT_TRUE(bFactoryRan.load());
				if constexpr (!std::is_void_v<T>) EXPECT_EQ(Shared.GetResult(), 42);
			}
		};
		auto Value = Tasks::LaunchTask("SharedBorrowedValue", [] { return 42; });
		ASSERT_EQ(Value.WaitDetailed().TaskState, ETaskState::Succeeded);
		Check(Value);
		auto Void = Tasks::LaunchTask("SharedBorrowedVoid", [] {});
		ASSERT_EQ(Void.WaitDetailed().TaskState, ETaskState::Succeeded);
		Check(Void);
	}

	TEST(FTaskCompositionTests, NestedCancellationDoesNotCancelSharedDeferredChild)
	{
		Tasks::FCancellationToken ParentCancellation, ChildCancellation;
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		auto Root = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [] {});
		std::atomic<int> Called = 0;
		FThreadEvent Spawned;
		Tasks::TTask<int> Inner;
		auto Outer = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&, Root](){
			EXPECT_FALSE(ParentCancellation.IsCanceled());
			Inner = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred, {}, [&] {
				EXPECT_FALSE(ChildCancellation.IsCanceled()); ++Called; return 12;
			});
			Tasks::AddNested(Inner);
			Spawned.Trigger();
		}, Root);
		ASSERT_TRUE(Spawned.WaitFor(1.0));
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::WaitDetailed(Outer).WaitStatus);
		ParentCancellation.Cancel();
		EXPECT_EQ(0, Called.load());
		EXPECT_NE(ETaskState::Canceled, Inner.GetState());
		PumpGameThreadDeferredWork();
		EXPECT_EQ(ETaskState::Succeeded, WaitOffGameThread(Outer).TaskState);
		EXPECT_EQ(12, Inner.GetResult());
		EXPECT_EQ(1, Called.load());
	}

	TEST(FTaskExecutorTests, CpuRootsAndDependentsHonorPriorityWithBoundedOldestService)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started;
		FThreadEvent Release;
		auto Blocker = Durin::Tasks::FTask(Tasks::LaunchTask("PriorityBlocker", [&] { Started.Trigger(); Release.Wait(); }));
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::vector<int> Order;
		FThreadEvent LastHighRan;
		std::vector<Tasks::TTask<void>> Completions;
		Tasks::FTaskExecutionOptions Options;
		Options.Priority = ETaskPriority::Low;
		auto Low = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, Options, [&] { Order.push_back(-1); });
		Completions.push_back(std::move(Low));
		Tasks::FTaskEvent Source("PrioritySignal", false);
		const Tasks::FTask Input = Source;
		Options.Priority = ETaskPriority::High;
		auto Edge = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, Options, [&, Input](){ Order.push_back(99); }, Input);
		Completions.push_back(std::move(Edge));
		Source.Trigger();
		for (int Index = 0; Index < 24; ++Index)
		{
			auto High = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, Options, [&, Index] {
				Order.push_back(Index);
				if (Index == 23) LastHighRan.Trigger();
			});
			Completions.push_back(std::move(High));
		}
		Release.Trigger();
		ASSERT_TRUE(LastHighRan.WaitFor(2));
		Tasks::WaitDetailed(Completions);
		ASSERT_EQ(26u, Order.size());
		EXPECT_EQ(99, Order.front());
		EXPECT_LE(std::ranges::find(Order, -1) - Order.begin(), 7);
		std::erase(Order, -1);
		for (int Index = 0; Index < 24; ++Index) EXPECT_EQ(Index, Order[Index + 1]);
	}
	TEST(FTaskExecutorTests, BlockingIoChildWaitAndCrossExecutorShutdownDrain)
	{
		FEngineThreadPoolTestGuard Guard;
		for (bool Drain : {true, false})
		{
			ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .NumBlockingIOThreads = 1}));
			auto Parent = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [] {
				auto Child = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [] { return 11; });
				Tasks::AddNested(Child);

				auto Task = std::move(Child);
				EXPECT_EQ(ETaskState::Succeeded, Tasks::WaitDetailed(Task).TaskState);
			});
			auto ParentTask = std::move(Parent);
			EXPECT_EQ(ETaskState::Succeeded, Tasks::WaitDetailed(ParentTask).TaskState);
			FThreadEvent Started, Release;
			auto IO = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [&] { Started.Trigger(); Release.Wait(); return 3; });
			auto Task = std::move(IO);
			auto Edge = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [Task]() {
			auto Value = Task.GetResult(); return Value + 1; }, Task);
			auto Tail = std::move(Edge);
			ASSERT_TRUE(Started.WaitFor(1.0));
			std::thread Shutdown([&] { ShutdownTaskScheduler(Drain); });
			Release.Trigger();
			Shutdown.join();
			EXPECT_TRUE(Tail.IsCompleted());
			if (Drain) EXPECT_EQ(ETaskState::Succeeded, Tail.GetState());
			EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().BlockingIOTrackedTasks);
		}
	}

	TEST(FTaskExecutorTests, ParallelPoliciesValidateBatchAndPreserveNestedSerialExecution)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		std::atomic<uint64> Count = 0;
		EXPECT_EQ(ETaskState::Invalid, Tasks::ParallelFor("InvalidBatch", 8, [](uint64) {},
			{.Policy = Tasks::EParallelForPolicy::ExplicitBatch}).State);
		auto Serial = Tasks::ParallelFor("Serial", 8192, [&](uint64) { ++Count; }, {.Policy = Tasks::EParallelForPolicy::Serial});
		EXPECT_EQ(1u, Serial.ChunkCount);
		auto Explicit = Tasks::ParallelFor("Explicit", 8, [&](uint64) { ++Count; }, {.Policy = Tasks::EParallelForPolicy::ExplicitBatch, .BatchSize = 1});
		EXPECT_GT(Explicit.ChunkCount, 1u);
		auto Auto = Tasks::ParallelFor("AutoNested", 16384, [&](uint64) {
			auto Nested = Tasks::ParallelFor("Nested", 1, [&](uint64) { ++Count; });
			EXPECT_EQ(1u, Nested.ChunkCount);
		});
		EXPECT_GT(Auto.ChunkCount, 1u);
		EXPECT_EQ(24584u, Count.load());
	}
	TEST(FTaskExecutorTests, IoExecutorRetainsHelpingAuthorityInsideCpuWork)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .NumBlockingIOThreads = 1}));
		FThreadEvent CpuBlocked, ReleaseCpu, Finished;
		auto Blocker = Durin::Tasks::FTask(Tasks::LaunchTask("HoldCpu", [&] { CpuBlocked.Trigger(); ReleaseCpu.Wait(); }));
		ASSERT_TRUE(CpuBlocked.WaitFor(1.0));
		auto Parent = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [&] {
			auto Child = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [] {
				auto IO = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [] {});
				Tasks::AddNested(IO);
				auto Task = std::move(IO);
				EXPECT_EQ(ETaskState::Succeeded, Tasks::WaitDetailed(Task).TaskState);
			});
			Tasks::AddNested(Child);
			auto Task = std::move(Child);
			EXPECT_EQ(ETaskState::Succeeded, Tasks::WaitDetailed(Task).TaskState);
			Finished.Trigger();
		});
		const bool CompletedWhileCpuBlocked = Finished.WaitFor(1.0);
		ReleaseCpu.Trigger();
		EXPECT_TRUE(CompletedWhileCpuBlocked);
		WaitTask(Blocker);
	}
	TEST(FTaskExecutorTests, StorageEstimateIncludesConcreteCapturesAndResultStorage)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		using FLargeValue = std::array<std::byte, 8192>;
		Tasks::FTaskEvent Ready("ResultStorageReady", false);
		auto Small = Tasks::LaunchTask("SmallResult", [] { return 1; }, Ready);
		auto Large = Tasks::LaunchTask("LargeResult", [] { return FLargeValue{}; }, Ready);
		EXPECT_GT(Large.GetDiagnostics().EstimatedStorageBytes, Small.GetDiagnostics().EstimatedStorageBytes);

		Tasks::FTaskEvent Gate("StorageGate", false);
		const auto Prerequisites = Tasks::Prerequisites(Gate);
		auto SmallCapture = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.Prerequisites = {Prerequisites.begin(), Prerequisites.end()}},
			[Value = std::array<std::byte, 16>{}] { EXPECT_EQ(std::byte{}, Value.front()); });
		auto LargeCapture = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.Prerequisites = {Prerequisites.begin(), Prerequisites.end()}},
			[Value = FLargeValue{}] { EXPECT_EQ(std::byte{}, Value.front()); });
		EXPECT_GT(LargeCapture.GetDiagnostics().EstimatedStorageBytes, SmallCapture.GetDiagnostics().EstimatedStorageBytes);
		Gate.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(LargeCapture).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(SmallCapture).TaskState);
		Ready.Trigger();
		EXPECT_EQ(Small.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Large.WaitDetailed().TaskState, ETaskState::Succeeded);
	}

	TEST(FTaskExecutorTests, DeferredDependenciesDoNotChargeSharedInputStorageAgain)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		using FLargeValue = std::array<std::byte, 8192>;
		Tasks::FTaskEvent Ready("SharedLargeResult", false);
		auto Input = Tasks::LaunchTask("LargeResult", [] { return FLargeValue{}; }, Ready);
		auto Inspect = [Input] { return Input.GetResultShared(); };
		auto Observer = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred, {}, Inspect,
			Tasks::Prerequisites(Input, Input));
		EXPECT_EQ(sizeof(Inspect), Observer.GetDiagnostics().EstimatedPayloadBytes);
		EXPECT_LT(Observer.GetDiagnostics().EstimatedPayloadBytes, sizeof(FLargeValue));
		Ready.Trigger();
		ASSERT_EQ(Input.WaitDetailed().TaskState, ETaskState::Succeeded);
		// Public completion can precede dependent dispatch by the producer's worker.
		FThreadEvent ProducerRetired;
		auto Probe = Tasks::LaunchTask("LargeProducerRetired", [&] { ProducerRetired.Trigger(); });
		ASSERT_TRUE(ProducerRetired.WaitFor(2)); Probe.Wait();
		PumpGameThreadDeferredWork({.bUnlimited = true});
		ASSERT_TRUE(Observer.IsCompleted());
		EXPECT_EQ(&Input.GetResult(), Observer.GetResult().get());

	}

	TEST(FTaskAcceptanceTests, QueuesAboveFormerDefaultsAndDrainsWithExecutorAffinity)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		FThreadEvent Started, Release;
		std::atomic<uint32> WorkerCalls = 0, DeferredCalls = 0;
		auto Blocker = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&] { Started.Trigger(); (void)Release.WaitFor(20.0); });
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::vector<Tasks::TTask<void>> Workers, Deferred;
		for (uint32 Index = 0; Index < 17000; ++Index)
			Workers.push_back(Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&] {
				EXPECT_FALSE(IsInGameThread()); ++WorkerCalls;
			}));
		for (uint32 Index = 0; Index < 1100; ++Index)
			Deferred.push_back(Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred, {}, [&] {
				EXPECT_TRUE(IsInGameThread()); ++DeferredCalls;
			}));
		const auto Peak = GetTaskSchedulerDiagnostics();
		EXPECT_GT(Peak.CurrentTrackedTaskCount, 16384u);
		EXPECT_EQ(1100u, GetGameThreadDeferredWorkQueueDiagnostics().QueueDepth);
		for (const auto& Storage : Peak.ExecutorStorage)
			std::cout << "Task storage nodes=" << Storage.CurrentNodes << " peak-bytes=" << Storage.PeakBytes
				<< " pending=" << Storage.CurrentPendingNodes << " pending-peak-bytes=" << Storage.PeakPendingBytes
				<< " running=" << Storage.CurrentRunningBodies << " running-peak=" << Storage.PeakRunningBodies << '\n';
		EXPECT_EQ(17000u, Peak.ExecutorStorage[0].CurrentPendingNodes);
		EXPECT_EQ(1u, Peak.ExecutorStorage[0].CurrentRunningBodies);
		EXPECT_EQ(1100u, Peak.ExecutorStorage[1].CurrentPendingNodes);
		EXPECT_GT(GetGameThreadDeferredWorkQueueDiagnostics().OverloadCrossings, 0u);
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS Memory{};
		Memory.cb = sizeof(Memory);
		if (K32GetProcessMemoryInfo(GetCurrentProcess(), &Memory, sizeof(Memory)))
			std::cout << "Saturation process peak working set bytes=" << Memory.PeakWorkingSetSize << '\n';
#endif
		Release.Trigger();
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		EXPECT_EQ(17000u, WorkerCalls.load());
		EXPECT_EQ(1100u, DeferredCalls.load());
		for (auto& Task : Workers) EXPECT_EQ(ETaskState::Succeeded, Task.GetState());
		for (auto& Task : Deferred) EXPECT_EQ(ETaskState::Succeeded, Task.GetState());
		const auto Final = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(0u, Final.CurrentTrackedTaskCount);
		for (const auto& Storage : Final.ExecutorStorage) { EXPECT_EQ(0u, Storage.CurrentNodes); EXPECT_EQ(0u, Storage.CurrentBytes); EXPECT_EQ(0u, Storage.CurrentPendingNodes); EXPECT_EQ(0u, Storage.CurrentPendingBytes); EXPECT_EQ(0u, Storage.CurrentRunningBodies); }
	}

	TEST(FTaskAcceptanceTests, SmallThresholdIncludesDependentTasksAndIoChildren)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .TaskWarningThreshold = 1,
			.NumBlockingIOThreads = 1, .BlockingIOWarningThreshold = 1}));
		Tasks::FTaskEvent Gate("ThresholdReady", false);
		auto Shared = LaunchOutcome(Gate, 10);
		std::vector<Tasks::TTask<int>> Inputs;
		for (int Index = 0; Index < 32; ++Index)
			Inputs.push_back(Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [Index, Shared]() {
			auto Value = Shared.GetResult(); return Value + Index; }, Shared));
		auto All = Tasks::LaunchTask("GatherThresholdResults", [Inputs] {
			std::vector<int> Values;
			for (const auto& Input : Inputs) Values.push_back(Input.GetResult());
			return Values;
		}, Inputs);
		auto Io = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [] {
			auto Child = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {}, [] { return 9; });
			Tasks::AddNested(Child);
			return std::move(Child.GetResult());
		});
		Gate.Trigger();
		EXPECT_EQ(9, std::move(Io.GetResult()));
		auto Values = std::move(All.GetResult());
		ASSERT_EQ(32u, Values.size());
		EXPECT_EQ(41, Values.back());
	}

	TEST(FTaskAcceptanceTests, ExplicitCancellationBeforeShutdownExecutesQueuedBodiesAndRetiresCapturesOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .TaskWarningThreshold = 1}));
		Tasks::FCancellationToken GroupCancellation;
		FThreadEvent Started, Release;
		auto Blocker = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&] { Started.Trigger(); (void)Release.WaitFor(5.0); });
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::atomic<int> Destroyed = 0, Called = 0;
		struct FPayload { std::atomic<int>* Count; ~FPayload() { ++*Count; } };
		std::vector<Tasks::TTask<void>> Queued;
		for (int Index = 0; Index < 64; ++Index)
			Queued.push_back(Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {},
				[Payload = std::make_unique<FPayload>(&Destroyed), &Called, &GroupCancellation] {
					EXPECT_TRUE(GroupCancellation.IsCanceled()); ++Called;
				}));
		GroupCancellation.Cancel();
		Release.Trigger();
		ShutdownTaskScheduler(false);
		EXPECT_EQ(64, Called.load());
		EXPECT_EQ(64, Destroyed.load());
		for (auto& Task : Queued) EXPECT_EQ(ETaskState::Succeeded, Task.GetState());
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount);
	}

	TEST(FTaskCompositionTests, DroppedAndThrowingSharedResultsReleaseExactlyOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		struct FValue { std::atomic<int>& Count; ~FValue() { ++Count; } };
		std::atomic<int> Destroyed = 0;
		{
			auto Dropped = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&] {
				return std::unique_ptr<FValue>(new FValue{Destroyed});
			});
			ASSERT_EQ(ETaskState::Succeeded, Dropped.WaitDetailed().TaskState);
		}
		ShutdownTaskScheduler(true);
		EXPECT_EQ(1, Destroyed.load());
		ASSERT_TRUE(InitializeTaskScheduler(1));
		{
			auto Input = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [&] {
				return std::unique_ptr<FValue>(new FValue{Destroyed});
			});
			auto Sink = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [Input]() { throw std::runtime_error("shared sink failed"); }, Input);
			ASSERT_EQ(ETaskState::Failed, Sink.WaitDetailed().TaskState);
			EXPECT_EQ(1, Destroyed.load()); // A failing observer does not discard its input.
		}
		ShutdownTaskScheduler(true);
		EXPECT_EQ(2, Destroyed.load());
	}

	TEST(FTaskCompositionTests, OrdinaryPrerequisitesWaitForAllOutcomesWithoutPropagatingFailure)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		for (uint32 Case = 0; Case < 4; ++Case)
		{
			Tasks::FTaskEvent Ready("PrimaryOutcome", false);
			auto Input = LaunchOutcome(Ready, 0, (Case & 1) ? ETaskState::Canceled : ETaskState::Failed);
			Tasks::FTaskEvent Gate("CompletionPrerequisiteGate", false);
			auto Complete = [&] { Ready.Trigger(); EXPECT_EQ(Input.WaitDetailed().WaitStatus, ETaskWaitStatus::Completed); };
			if (Case & 2) Complete();
			const auto Dependencies = Tasks::Prerequisites(Input, Gate);
			std::atomic<uint32> Calls = 0;
			auto Task = Tasks::LaunchTask("CompletionPrerequisites", [&] { ++Calls; return 42; }, Dependencies);
			if (!(Case & 2)) Complete();
			EXPECT_EQ(Task.GetState(), ETaskState::Waiting);
			EXPECT_EQ(Calls, 0u);
			Gate.Trigger();
			ASSERT_EQ(Task.WaitDetailed().TaskState, ETaskState::Succeeded);
			EXPECT_EQ(Task.GetResult(), 42);
			EXPECT_EQ(Calls, 1u);
		}
	}

	TEST(FTaskAcceptanceTests, ParallelForQueuesAllChunksBeyondWarningThreshold)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 2, .TaskWarningThreshold = 1}));
		std::atomic<int> Active = 0, Destroyed = 0;
		struct FCapture { std::atomic<int>& Count; ~FCapture() { ++Count; } };
		const auto Result = ParallelForCancelable("UnboundedChunks", 3,
			[Capture = std::shared_ptr<FCapture>(new FCapture{Destroyed}), &Active]
			(uint64, const FParallelForCancellationToken&) { ++Active; --Active; }, {.MinBatchSize = 1});
		EXPECT_EQ(ETaskState::Succeeded, Result.State);
		EXPECT_EQ(0, Active.load());
		EXPECT_EQ(1, Destroyed.load());
		const auto Diagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(2u, Diagnostics.CompletedTaskCount);
		EXPECT_EQ(0u, Diagnostics.CurrentTrackedTaskCount);
	}


}
