#include <gtest/gtest.h>
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Threading/ThreadEvent.h"
#include "Threading/TaskComposition.h"
#include "Threading/TaskPipe.h"

namespace Durin
{
	class FTaskCloseTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
			ShutdownTaskScheduler(false);
		}
		auto TearDown() -> void override { ShutdownTaskScheduler(false); }
	};

	TEST_F(FTaskCloseTests, PipeSerializesReadyTasksInLaunchOrder)
	{
		static_assert(!std::is_copy_constructible_v<Tasks::FPipe> && !std::is_move_constructible_v<Tasks::FPipe>);
		ASSERT_TRUE(InitializeTaskScheduler(4));
		Tasks::FPipe Pipe("SerialPipe");
		EXPECT_STREQ(Pipe.GetDebugName(), "SerialPipe");
		EXPECT_FALSE(Pipe.HasWork());
		EXPECT_TRUE(Pipe.WaitUntilEmpty(0));
		FThreadEvent Started, Release;
		auto First = Pipe.Launch("First", [&] { Started.Trigger(); Release.Wait(); });
		const bool bStarted = Started.WaitFor(5);
		std::atomic<uint32> Active = 0, Next = 0;
		std::vector<Tasks::FTask> Tasks;
		for (uint32 Index = 0; Index < 256; ++Index)
			Tasks.push_back(Pipe.Launch("Ordered", [&, Index] {
				EXPECT_EQ(Active.fetch_add(1), 0u);
				EXPECT_TRUE(Pipe.IsInContext());
				EXPECT_EQ(Next.fetch_add(1), Index);
				std::this_thread::yield();
				EXPECT_EQ(Active.fetch_sub(1), 1u);
			}, Index % 2 ? ETaskPriority::High : ETaskPriority::Normal));
		EXPECT_TRUE(Pipe.HasWork());
		EXPECT_FALSE(Pipe.WaitUntilEmpty(0.001));
		Release.Trigger();
		EXPECT_TRUE(bStarted);
		EXPECT_TRUE(Pipe.WaitUntilEmpty(5));
		Tasks::Wait(Tasks);
		EXPECT_EQ(Next.load(), 256u);
		EXPECT_FALSE(Pipe.IsInContext());
	}

	TEST_F(FTaskCloseTests, PipePrerequisitesDoNotBlockReadyWork)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FPipe Pipe;
		Tasks::FTaskEvent Gate("PipePrerequisite", false);
		std::atomic<uint32> Sequence = 0;
		auto Delayed = Pipe.Launch("Delayed", [&] { EXPECT_EQ(Sequence.fetch_add(1), 1u); return 42; }, Gate);
		auto Ready = Pipe.Launch("Ready", [&] { EXPECT_EQ(Sequence.fetch_add(1), 0u); });
		EXPECT_EQ(Ready.Wait(2).WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_FALSE(Delayed.IsCompleted());
		EXPECT_TRUE(Pipe.HasWork());
		EXPECT_FALSE(Pipe.WaitUntilEmpty(0.001));
		Gate.Trigger();
		EXPECT_EQ(Delayed.GetResult(), 42);
		EXPECT_TRUE(Pipe.WaitUntilEmpty(2));
	}

	TEST_F(FTaskCloseTests, PipeConcurrentProducersExecuteExactlyOnceWithoutOverlap)
	{
		ASSERT_TRUE(InitializeTaskScheduler(4));
		Tasks::FPipe Pipe;
		Tasks::FTaskEvent Gate("ConcurrentPipePrerequisite", false);
		std::atomic<uint32> Active = 0, Runs = 0, Seeded = 0;
		std::array<std::thread, 4> Producers;
		for (auto& Producer : Producers)
			Producer = std::thread([&] {
				for (uint32 Index = 0; Index < 256; ++Index)
				{
					auto Body = [&] {
						EXPECT_EQ(Active.fetch_add(1), 0u);
						std::this_thread::yield();
						++Runs;
						EXPECT_EQ(Active.fetch_sub(1), 1u);
					};
					if (Index % 2) Pipe.Launch("ConcurrentReadyPipe", std::move(Body));
					else Pipe.Launch("ConcurrentDelayedPipe", std::move(Body), Gate, ETaskPriority::High);
					if (Index == 0) ++Seeded;
				}
			});
		while (Seeded.load() != Producers.size()) std::this_thread::yield();
		Gate.Trigger();
		for (auto& Producer : Producers) Producer.join();
		EXPECT_TRUE(Pipe.WaitUntilEmpty(5));
		EXPECT_EQ(Runs.load(), 1024u);
	}

	TEST_F(FTaskCloseTests, PipeWaitIncludesNestedCompletionAndOtherPipesProgress)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FPipe Pipe, Other;
		Tasks::FTaskEvent Nested("PipeNested", false);
		FThreadEvent BodyFinished;
		auto Parent = Pipe.Launch("Parent", [&] { Tasks::AddNested(Nested); BodyFinished.Trigger(); });
		const bool bBodyFinished = BodyFinished.WaitFor(5);
		auto Next = Pipe.Launch("AfterNested", [] {});
		auto Independent = Other.Launch("OtherPipe", [&] {
			EXPECT_TRUE(Other.IsInContext());
			EXPECT_FALSE(Pipe.IsInContext());
		});
		EXPECT_EQ(Independent.Wait(2).WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_FALSE(Parent.IsCompleted());
		EXPECT_FALSE(Next.IsCompleted());
		EXPECT_FALSE(Pipe.WaitUntilEmpty(0.001));
		Nested.Trigger();
		EXPECT_TRUE(bBodyFinished);
		EXPECT_EQ(Next.Wait(2).WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_TRUE(Pipe.WaitUntilEmpty(2));
	}

	TEST_F(FTaskCloseTests, PipeTailWaitRetractsPredecessorsWithOneOccupiedWorker)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started, Release;
		auto Blocker = Tasks::LaunchTask("OccupiedWorker", [&] { Started.Trigger(); Release.Wait(); });
		const bool bStarted = Started.WaitFor(5);
		Tasks::FPipe Pipe;
		std::atomic<uint32> Runs = 0;
		Tasks::TTask<void> Tail;
		for (uint32 Index = 0; Index < 64; ++Index)
			Tail = Pipe.Launch("RetractedPipe", [&] { ++Runs; });
		const auto Result = Tail.Wait(2);
		Release.Trigger();
		Blocker.Wait();
		EXPECT_TRUE(bStarted);
		EXPECT_EQ(Result.WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_EQ(Runs.load(), 64u);
		EXPECT_TRUE(Pipe.WaitUntilEmpty(2));
	}

	TEST_F(FTaskCloseTests, PipeFailureCancellationAndContextPreserveTaskSemantics)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FPipe Pipe;
		Tasks::FTaskEvent Gate("PipeCancellation", false);
		FTaskCancellationSource Cancellation;
		Cancellation.RequestCancellation();
		auto Canceled = Pipe.Launch("CooperativePipe", [&](const FTaskCancellationToken& Token) {
			EXPECT_TRUE(Token.IsCancellationRequested());
			EXPECT_TRUE(Pipe.IsInContext());
			EXPECT_FALSE(Pipe.WaitUntilEmpty(0));
			return std::make_unique<int>(17);
		}, Gate, {.Cancellation = Cancellation.GetToken()});
		FThreadEvent ThrowStarted, AllowThrow;
		auto Failed = Pipe.Launch("ThrowingPipe", [&] {
			ThrowStarted.Trigger();
			AllowThrow.Wait();
			throw std::runtime_error("pipe failure");
		});
		const bool bThrowStarted = ThrowStarted.WaitFor(5);
		auto Next = Pipe.Launch("AfterFailure", [] { return 23; });
		AllowThrow.Trigger();
		EXPECT_TRUE(bThrowStarted);
		EXPECT_EQ(Next.GetResult(), 23);
		EXPECT_EQ(Failed.GetState(), ETaskState::Failed);
		Gate.Trigger();
		EXPECT_EQ(*Canceled.GetResult(), 17);
		EXPECT_EQ(Canceled.GetState(), ETaskState::Succeeded);
		EXPECT_TRUE(Pipe.WaitUntilEmpty(2));
	}

	TEST_F(FTaskCloseTests, PipeSameContextWaitAndNestedCyclesAreRejected)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FPipe Pipe;
		Tasks::TTask<void> Child;
		auto Parent = Pipe.Launch("PipeParent", [&] {
			Child = Pipe.Launch("PipeChild", [] {});
			EXPECT_EQ(Child.Wait().WaitStatus, ETaskWaitStatus::DependencyCycle);
			const auto Error = Private::FTaskRuntimeAccess::AddNested(Child);
			ASSERT_TRUE(Error.has_value());
			EXPECT_EQ(Error->Code, Tasks::ETaskConstructionErrorCode::DependencyCycle);
			EXPECT_FALSE(Pipe.WaitUntilEmpty(0.01));
			EXPECT_TRUE(Pipe.IsInContext());
		});
		Parent.Wait();
		EXPECT_EQ(Child.Wait(2).WaitStatus, ETaskWaitStatus::Completed);
		EXPECT_TRUE(Pipe.WaitUntilEmpty(2));
	}

	TEST_F(FTaskCloseTests, PipeResultOutlivesDrainedPipeAndShutdownDrainsDelayedWork)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::TTask<std::unique_ptr<int>> Result;
		{
			Tasks::FPipe Pipe;
			Tasks::FTaskEvent Gate("PipeShutdown", false);
			Result = Pipe.Launch("SurvivingResult", [] { return std::make_unique<int>(61); }, Gate);
			std::thread Signaler([&] { Gate.Trigger(); });
			ShutdownTaskScheduler(false);
			Signaler.join();
			EXPECT_TRUE(Pipe.WaitUntilEmpty(0));
		}
		EXPECT_EQ(*Result.GetResult(), 61);
	}

	TEST_F(FTaskCloseTests, DeepNestedTasksRetireCapturesBeforeShutdownCompletes)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		constexpr uint32 Depth = 2048;
		std::atomic<uint32> Retired = 0;
		struct FOwner { std::atomic<uint32>& Count; ~FOwner() { ++Count; } };
		Tasks::FTaskEvent Gate("NestedChainGate", false);
		Tasks::FTask Current = Gate;
		for (uint32 Index = 0; Index < Depth; ++Index)
		{
			Current = Tasks::LaunchTask("NestedChain", [Child = Current,
				Owner = std::shared_ptr<FOwner>(new FOwner{Retired})] {
				Tasks::AddNested(Child);
			});
		}
		EXPECT_FALSE(Current.IsCompleted());
		// Bodies may release captures before their nested completion dependencies.
		Gate.Trigger();
		EXPECT_EQ(Current.Wait().TaskState, ETaskState::Succeeded);
		ShutdownTaskScheduler(false);
		EXPECT_EQ(Retired.load(), Depth);
	}

	namespace
	{
		template<typename T> concept CHasTaskResult = requires(const T& Task) { Task.GetResult(); };
		template<typename T> concept CHasSharedTaskResult = requires(const T& Task) { Task.GetResultShared(); };
	}
	static_assert(!CHasTaskResult<Tasks::TTask<void>>);
	static_assert(!CHasSharedTaskResult<Tasks::TTask<void>>);
	static_assert(CHasTaskResult<Tasks::TTask<std::monostate>>);
	static_assert(std::same_as<Tasks::Detail::TResultState<void>, std::nullptr_t>);

	TEST_F(FTaskCloseTests, VoidTasksPublishOnlyAfterCaptureAndNestedRetirement)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("VoidNestedGate", false);
		auto Capture = std::make_shared<int>(42);
		std::weak_ptr<int> WeakCapture = Capture;
		auto Parent = Tasks::LaunchTask("VoidParent", [Capture = std::move(Capture), Gate] { Tasks::AddNested(Gate); });
		EXPECT_FALSE(Parent.TryRetractAndExecute());
		// A queued barrier establishes that the parent body and captures have retired.
		auto Barrier = Tasks::LaunchTask("VoidBarrier", [] {});
		Barrier.Wait();
		EXPECT_FALSE(Parent.IsCompleted());
		Gate.Trigger();
		EXPECT_EQ(Parent.Wait().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(WeakCapture.expired());
		auto Edge = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [Parent](){}, Parent);
		auto Observed = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [Edge]() {
			const auto& Task = Edge;
			EXPECT_EQ(Task.GetState(), ETaskState::Succeeded);
		}, Edge);
		EXPECT_EQ(Observed.Wait().TaskState, ETaskState::Succeeded);
	}

	TEST_F(FTaskCloseTests, VoidTasksPreserveProducerOutcomes)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (auto Outcome : {ETaskState::Succeeded, ETaskState::Failed, ETaskState::Canceled})
		{
			Tasks::FTaskEvent Ready("VoidOutcome", false);
			auto Task = Tasks::LaunchTask("VoidOutcomeTask", [Outcome] {
				if (Outcome == ETaskState::Failed) throw std::runtime_error("void task failure");
				if (Outcome == ETaskState::Canceled) Private::FTaskRuntimeAccess::SetCurrentCanceledOutcome();
			}, Ready);
			EXPECT_FALSE(Task.IsCompleted());
			Ready.Trigger();
			EXPECT_EQ(Task.Wait().TaskState, Outcome);
		}
	}

	TEST_F(FTaskCloseTests, ClosedAdmissionDispatchesEventPublicationAndJoinsDroppedProducerCaptures)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTaskEvent Signal("EarlyProducerSignal");
		FThreadEvent Started, Release;
		std::atomic<bool> Retired = false;
		struct FRetirement { std::atomic<bool>& Flag; ~FRetirement() { Flag = true; } };
		auto Producer = Tasks::LaunchTask("TrackedProducer", [&, Signal,
			Capture = std::shared_ptr<FRetirement>(new FRetirement{Retired})]() mutable {
			Started.Trigger();
			// Observe the actual admission transition, not a timing approximation.
			while (IsTaskSchedulerRunning()) std::this_thread::yield();
			Signal.Trigger();
			Release.Wait();
		});
		auto Publication = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred,
			{.DebugName = "PreadmittedPublication"}, [&] {
				EXPECT_FALSE(IsTaskSchedulerRunning());
				EXPECT_FALSE(Retired.load());
				Release.Trigger();
			}, Signal);
		Started.Wait(); Producer = {};
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		EXPECT_TRUE(Retired.load());
		EXPECT_TRUE(Publication.IsCompleted());
		const auto Diagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(0u, Diagnostics.NonterminalTaskCount);
		EXPECT_EQ(0u, Diagnostics.ActiveWorkerCount);
		EXPECT_EQ(0u, Diagnostics.QueueDepth);
		EXPECT_EQ(0u, GetGameThreadDeferredWorkQueueDiagnostics().QueueDepth);
	}

	TEST_F(FTaskCloseTests, DependentResultViewsAndErasedHandlesRetainNodesAfterShutdown)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		std::atomic<uint32> Retired = 0;
		struct FOwner { std::atomic<uint32>& Count; ~FOwner() { ++Count; } };
		auto Seed = Tasks::LaunchTask("OwnedSeed", [&] { return std::unique_ptr<FOwner>(new FOwner{Retired}); });
		auto All = Tasks::LaunchTask("RetainViews", [Seed] { return std::array{Seed.GetResultShared(), Seed.GetResultShared()}; }, Tasks::Prerequisites(Seed, Seed));
		auto Void = Tasks::LaunchTask("VoidInput", [] {});
		auto Mixed = Tasks::LaunchTask("RetainMixed", [Void, Seed] { return std::tuple{Tasks::FTask(Void), Seed.GetResultShared()}; }, Tasks::Prerequisites(Void, Seed));
		EXPECT_EQ(All.Wait().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Mixed.Wait().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(All.GetResult()[0], All.GetResult()[1]);
		EXPECT_EQ(All.GetResult()[0], std::get<1>(Mixed.GetResult()));
		EXPECT_TRUE(std::get<0>(Mixed.GetResult()).IsCompleted());
		auto View = All.GetResult().front();
		Tasks::FTask Retained = Seed;
		Seed = {}; All = {}; Mixed = {};
		ShutdownTaskScheduler(false);
		EXPECT_EQ(Retired.load(), 0u);
		Retained = {};
		EXPECT_EQ(Retired.load(), 0u);
		View.reset();
		EXPECT_EQ(Retired.load(), 1u);
	}

	TEST_F(FTaskCloseTests, WideDependenciesKeepAllRegistrations)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		constexpr uint32 Count = 4096;
		Tasks::FTaskEvent Gate("WideGate", false);
		FThreadEvent Registered;
		std::atomic<uint32> Runs = 0;
		std::vector<Tasks::FTask> Children;
		for (uint32 Index = 0; Index < Count; ++Index)
			Children.push_back(Tasks::LaunchTask("WideChild", [&] { ++Runs; }, Gate));
		auto Parent = Tasks::LaunchTask("WideParent", [&] {
			for (const auto& Child : Children) Tasks::AddNested(Child);
			Registered.Trigger();
		});
		EXPECT_FALSE(Parent.TryRetractAndExecute());
		const bool bRegistered = Registered.WaitFor(5);
		Gate.Trigger();
		EXPECT_TRUE(bRegistered);
		EXPECT_EQ(Parent.Wait().TaskState, ETaskState::Succeeded);
		EXPECT_EQ(Runs.load(), Count);
	}

	TEST_F(FTaskCloseTests, PreparedDependenciesDrainAfterProducersStop)
	{
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 2, .TaskWarningThreshold = 32}));
		Tasks::FTaskEvent Gate("PreparationCloseGate", false);
		FThreadEvent Start;
		std::atomic<uint32> Ready = 0, Accepted = 0, Runs = 0;
		std::array<std::thread, 4> Producers;
		for (auto& Producer : Producers)
			Producer = std::thread([&] {
				++Ready;
				Start.Wait();
				for (uint32 Index = 0; Index < 128; ++Index)
				{
					const auto Prerequisites = Tasks::Prerequisites(Gate);
					FTaskLaunchOptions Options;
					Options.Prerequisites = Prerequisites;
					auto Admission = Private::LaunchCancelableTaskWithCompletion("PreparedClose",
						[&](const FTaskCancellationToken&) { ++Runs; }, {}, Options);
					EXPECT_TRUE(Admission.IsValid());
					++Accepted;
				}
			});
		while (Ready.load() != Producers.size()) std::this_thread::yield();
		Start.Trigger();
		Gate.Trigger();
		for (auto& Producer : Producers) Producer.join();
		ShutdownTaskScheduler(true);
		EXPECT_EQ(Runs.load(), Accepted.load());
		EXPECT_EQ(GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount, 0u);
		// Reusing the event in another lifetime also exercises completed-node
		// registration after all speculative reservations have been rolled back.
		ASSERT_TRUE(InitializeTaskScheduler(1));
		EXPECT_EQ(Tasks::LaunchTask("AfterPreparedClose", [] {}, Gate).Wait().TaskState, ETaskState::Succeeded);
	}

	TEST_F(FTaskCloseTests, CombinedWaitInspectionPreservesCyclePrecedenceAndClosedRequirements)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent ClosedUnknown("ClosedUnknown");
		ClosedUnknown.Trigger();
		Tasks::FTaskEvent Gate("KnownProgress", false);
		Tasks::FTask Tail = Gate;
		for (uint32 Index = 0; Index < 128; ++Index)
			Tail = Tasks::LaunchTask("SharedInspectionGraph", [] {}, Tasks::Prerequisites(Tail, ClosedUnknown));
		auto Observer = Tasks::LaunchTask("CheckClosedRequirements", [&] {
			EXPECT_EQ(Tail.Wait(0.001).WaitStatus, ETaskWaitStatus::TimedOut);
			Gate.Trigger();
			EXPECT_EQ(Tail.Wait().TaskState, ETaskState::Succeeded);
		});
		Observer.Wait();

		Tasks::FTaskEvent Unknown("PendingUnknown");
		Tasks::FTaskEvent Cycle("CycleAndUnknown", false);
		FThreadEvent Bound;
		auto Parent = Tasks::LaunchTask("CyclePrecedence", [&] {
			Bound.Wait();
			EXPECT_EQ(Cycle.Wait().WaitStatus, ETaskWaitStatus::DependencyCycle);
		});
		Cycle.AddPrerequisites(Tasks::Prerequisites(Parent, Unknown));
		Cycle.Trigger();
		Bound.Trigger();
		Parent.Wait();
		Unknown.Trigger();
		EXPECT_TRUE(Cycle.IsCompleted());
	}

	TEST_F(FTaskCloseTests, CancellationRequestsPreserveDependenciesBodiesAndResults)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (bool Shared : {false, true})
		{
			FTaskCancellationSource Cancellation;
			if (Shared) Cancellation.RequestCancellation();
			Tasks::FTaskEvent Gate("CanceledPrerequisiteGate", false);
			uint32 Runs = 0;
			auto Task = Tasks::LaunchTask("CooperativeRoot", [&](const FTaskCancellationToken& Token) {
				EXPECT_TRUE(Token.IsCancellationRequested());
				++Runs;
				return std::make_unique<int>(42);
			}, {.Cancellation = Cancellation.GetToken(), .Prerequisites = Tasks::Prerequisites(Gate)});
			if (!Shared) EXPECT_TRUE(Tasks::Cancel(Task));
			EXPECT_TRUE(Task.IsCancellationRequested());
			EXPECT_FALSE(Task.IsCompleted());
			EXPECT_EQ(Task.Wait(0).WaitStatus, ETaskWaitStatus::TimedOut);
			EXPECT_EQ(Runs, 0u);
			auto Next = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {}, [Task](const FTaskCancellationToken& Token) {
				auto& Value = Task.GetResult();
				EXPECT_FALSE(Token.IsCancellationRequested());
				return *Value + 1;
			}, Task);
			Gate.Trigger();
			EXPECT_EQ(Next.GetResult(), 43);
			EXPECT_EQ(Runs, 1u);
			EXPECT_EQ(Task.GetState(), ETaskState::Succeeded);
			EXPECT_EQ(*Task.GetResult(), 42);
		}
	}

	TEST_F(FTaskCloseTests, CancelingNestedParentRetainsItsValueAndDoesNotCancelChild)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("CooperativeNestedGate", false);
		FThreadEvent BodyFinished;
		auto Parent = Tasks::LaunchTask("CooperativeNestedParent", [&] {
			Tasks::AddNested(Gate);
			BodyFinished.Trigger();
			return std::make_unique<int>(17);
		});
		ASSERT_TRUE(BodyFinished.WaitFor(1));
		EXPECT_TRUE(Tasks::Cancel(Parent));
		EXPECT_FALSE(Parent.IsCompleted());
		EXPECT_FALSE(Gate.IsCancellationRequested());
		Gate.Trigger();
		EXPECT_EQ(*Parent.GetResult(), 17);
		EXPECT_EQ(Parent.GetState(), ETaskState::Succeeded);
	}

	TEST_F(FTaskCloseTests, CancelShutdownDrainsAcceptedWorkerAndDeferredBodies)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		FThreadEvent Started;
		auto Running = Tasks::LaunchTask("CooperativeShutdownRunning", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (!Token.IsCancellationRequested() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
			EXPECT_TRUE(Token.IsCancellationRequested());
		});
		ASSERT_TRUE(Started.WaitFor(1));
		uint32 Runs = 0;
		auto Deferred = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred, {}, [&](const FTaskCancellationToken& Token) {
			EXPECT_TRUE(Token.IsCancellationRequested()); ++Runs;
		});
		auto Dependent = Tasks::LaunchTask("CooperativeShutdownDependent", [&](const FTaskCancellationToken& Token) {
			EXPECT_TRUE(Token.IsCancellationRequested()); ++Runs;
		}, Tasks::Prerequisites(Deferred));
		ShutdownTaskSystem(ETaskShutdownMode::Cancel);
		EXPECT_EQ(Runs, 2u);
		for (Tasks::FTask Task : {Tasks::FTask(Running), Tasks::FTask(Deferred), Tasks::FTask(Dependent)})
			EXPECT_EQ(Task.GetState(), ETaskState::Succeeded);
		EXPECT_EQ(GetTaskSchedulerDiagnostics().CurrentTrackedTaskCount, 0u);
	}

	TEST_F(FTaskCloseTests, ClosingHooksCanRegisterDependenciesAndMoreHooks)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Event("ClosingRegistration", false);
		Tasks::TTask<int> Dependent;
		Tasks::FTaskSubscription Late;
		uint32 Calls = 0;
		auto Subscription = Tasks::SubscribeTask(Event, [&](ETaskState) {
			EXPECT_FALSE(Event.IsCompleted());
			Dependent = Tasks::LaunchTask("RegisteredDuringClose", [] { return 42; }, Tasks::Prerequisites(Event));
			Late = Tasks::SubscribeTask(Event, [&](ETaskState) { ++Calls; });
		});
		Event.Trigger();
		EXPECT_TRUE(Event.IsCompleted());
		EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Dependent.GetResult(), 42);
	}

	TEST_F(FTaskCloseTests, NestedRetractionReleasesResultPinsAndNotifiesExecutionWaiters)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (bool ChildFirst : {false, true})
		{
			Tasks::FTaskEvent Outer("MixedDependencyTarget", false);
			Tasks::FTaskEvent Gate("MixedDependencyGate", false);
			Tasks::FTaskEvent ChildGate("MixedChildGate", false);
			std::weak_ptr<int> Weak;
			{
				auto Value = std::make_shared<int>(42);
				Weak = Value;
				auto Child = Tasks::LaunchTask("PinnedChild", [Value] { return Value; }, ChildGate);
				if (!ChildFirst) Outer.AddPrerequisites(Gate);
				Outer.AddPrerequisites(Child);
				if (ChildFirst) Outer.AddPrerequisites(Gate);
				ChildGate.Trigger();
				ASSERT_EQ(Child.Wait().TaskState, ETaskState::Succeeded);
			}
			EXPECT_FALSE(Weak.expired());
			auto Waiter = Tasks::LaunchTask("MixedDependencyWaiter", [&] {
				EXPECT_TRUE(Weak.expired());
				return 42;
			}, Tasks::Prerequisites(Outer, Gate));
			EXPECT_FALSE(Outer.TryRetractAndExecute());
			EXPECT_FALSE(Outer.TryRetractAndExecute());
			EXPECT_TRUE(Weak.expired());
			Outer.Trigger();
			EXPECT_FALSE(Outer.IsCompleted());
			Gate.Trigger();
			EXPECT_EQ(Waiter.GetResult(), 42);
			EXPECT_TRUE(Outer.IsCompleted());
		}
	}


	TEST_F(FTaskCloseTests, NodeNotificationsRevisitBlockedNestedGraphs)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (bool NestedParent : {false, true})
		{
			Tasks::FTaskEvent Gate("ReadinessGate", false);
			Tasks::FTaskEvent Event("NestedWaitTarget", false);
			auto Child = Tasks::LaunchTask("LateReadyChild", [] {}, Tasks::Prerequisites(Gate));
			Tasks::FTask Target;
			if (NestedParent)
			{
				Target = Tasks::LaunchTask("NestedWaitParent", [Child] { Tasks::AddNested(Child); });
			}
			else
			{
				Event.AddPrerequisites(Child);
				Target = Event;
			}
			Event.Trigger();
			FThreadEvent Waiting;
			auto Waiter = Tasks::LaunchTask("OnlyWorkerNodeWaiter", [&] {
				// This first wait expires with dependencies still blocked. Its signal
				// must not keep the next wait alive or hide the later ready transition.
				EXPECT_EQ(Target.Wait(0.005).WaitStatus, ETaskWaitStatus::TimedOut);
				Waiting.Trigger();
				EXPECT_EQ(Target.Wait(2).TaskState, ETaskState::Succeeded);
			});
			EXPECT_TRUE(Waiting.WaitFor(1));
			std::atomic<bool> UnrelatedRan = false;
			auto Unrelated = Tasks::LaunchTask("UnrelatedReadyNode", [&] { UnrelatedRan = true; });
			EXPECT_FALSE(UnrelatedRan.load());
			Gate.Trigger();
			EXPECT_EQ(Waiter.Wait(3).TaskState, ETaskState::Succeeded);
			EXPECT_TRUE(Child.IsCompleted());
			EXPECT_EQ(Unrelated.Wait(1).TaskState, ETaskState::Succeeded);
		}
	}

	TEST_F(FTaskCloseTests, DeepNestedAndHookClosePropagationUsesBoundedStack)
	{
		// Nested parent completion and inline terminal hooks are separate sources
		// of synchronous propagation. Both must tolerate chains beyond a native stack.
		constexpr uint32 Count = 8192;
		for (bool Nested : {false, true})
		{
			std::vector<Tasks::FTaskEvent> Events;
			Events.reserve(Count);
			for (uint32 Index = 0; Index < Count; ++Index) Events.emplace_back("DeepClose", false);
			for (uint32 Index = 0; Index + 1 < Count; ++Index)
			{
				if (Nested) Events[Index].AddPrerequisites(Events[Index + 1]);
				else
				{
					auto Hook = std::make_shared<Private::FTaskTerminalHook>();
					Hook->Function = [Next = Events[Index + 1]](ETaskState) mutable { Next.Trigger(); };
					Private::FTaskRuntimeAccess::BindTerminal(Events[Index], std::move(Hook));
				}
			}
			if (Nested)
			{
				for (uint32 Index = 0; Index + 1 < Count; ++Index) Events[Index].Trigger();
				EXPECT_FALSE(Events.front().IsCompleted());
				Events.back().Trigger();
			}
			else Events.front().Trigger();
			for (const auto& Event : Events) ASSERT_TRUE(Event.IsCompleted());
		}
	}

}
