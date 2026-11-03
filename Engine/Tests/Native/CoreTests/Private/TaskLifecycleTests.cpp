#include "TaskLifecycleTestSupport.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{

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
		EXPECT_EQ(Current.WaitDetailed().TaskState, ETaskState::Succeeded);
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
		EXPECT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Succeeded);
		EXPECT_TRUE(WeakCapture.expired());
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
			EXPECT_EQ(Task.WaitDetailed().TaskState, Outcome);
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
		auto Published = Tasks::LaunchTask("RetainView", [Seed] { return Seed.GetResultShared(); }, Seed);
		ASSERT_EQ(Published.WaitDetailed().TaskState, ETaskState::Succeeded);
		auto View = Published.GetResult();
		ASSERT_TRUE(View);
		EXPECT_TRUE(*View);
		Tasks::FTask Retained = Seed;
		Seed = {}; Published = {};
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
		EXPECT_EQ(Parent.WaitDetailed().TaskState, ETaskState::Succeeded);
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
		EXPECT_EQ(Tasks::LaunchTask("AfterPreparedClose", [] {}, Gate).WaitDetailed().TaskState, ETaskState::Succeeded);
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
			EXPECT_EQ(Tail.WaitDetailed(0.001).WaitStatus, ETaskWaitStatus::TimedOut);
			Gate.Trigger();
			EXPECT_EQ(Tail.WaitDetailed().TaskState, ETaskState::Succeeded);
		});
		Observer.Wait();

		Tasks::FTaskEvent Unknown("PendingUnknown");
		Tasks::FTaskEvent Cycle("CycleAndUnknown", false);
		FThreadEvent Bound;
		auto Parent = Tasks::LaunchTask("CyclePrecedence", [&] {
			Bound.Wait();
			EXPECT_EQ(Cycle.WaitDetailed().WaitStatus, ETaskWaitStatus::DependencyCycle);
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
			}, {.Cancellation = Cancellation.GetToken(), .Prerequisites = {Gate}});
			if (!Shared) EXPECT_TRUE(Tasks::Cancel(Task));
			EXPECT_TRUE(Task.IsCancellationRequested());
			EXPECT_FALSE(Task.IsCompleted());
			EXPECT_EQ(Task.WaitDetailed(0).WaitStatus, ETaskWaitStatus::TimedOut);
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
		uint32 Calls = 0;
		auto Hook = std::make_shared<Private::FTaskBeforeCompletionHook>();
		Hook->Function = [&](ETaskState) {
			EXPECT_FALSE(Event.IsCompleted());
			Dependent = Tasks::LaunchTask("RegisteredDuringClose", [] { return 42; }, Tasks::Prerequisites(Event));
			auto Late = std::make_shared<Private::FTaskBeforeCompletionHook>();
			Late->Function = [&](ETaskState) { EXPECT_FALSE(Event.IsCompleted()); ++Calls; };
			Private::FTaskRuntimeAccess::BindBeforeCompletion(Event, std::move(Late));
		};
		Private::FTaskRuntimeAccess::BindBeforeCompletion(Event, std::move(Hook));
		Event.Trigger();
		EXPECT_TRUE(Event.IsCompleted());
		EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Dependent.GetResult(), 42);
	}

	TEST_F(FTaskCloseTests, PublicCompletionObserversReadResultsAndWaitForReleasedDependents)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Gate("CompletionObserverGate", false);
		auto Task = Tasks::LaunchTask("ObservedResult", [] { return std::make_unique<int>(42); }, Gate);
		auto Dependent = Tasks::LaunchTask("ObservedDependent", [] { return 43; }, Task);
		std::atomic<bool> Settled = false;
		auto Hook = std::make_shared<Private::FTaskBeforeCompletionHook>();
		Hook->Function = [&](ETaskState) {
			EXPECT_FALSE(Task.IsCompleted());
			EXPECT_FALSE(Task.GetResultShared());
			Settled = true;
		};
		Private::FTaskRuntimeAccess::BindBeforeCompletion(Task, std::move(Hook));
		FThreadEvent Observed;
		auto Subscription = Task.Subscribe([&, Task, Dependent](ETaskState Outcome) {
			EXPECT_TRUE(Settled.load());
			EXPECT_TRUE(Task.IsCompleted());
			EXPECT_EQ(Outcome, ETaskState::Succeeded);
			EXPECT_TRUE(Task.Wait());
			EXPECT_EQ(*Task.GetResult(), 42);
			const auto View = Task.GetResultShared();
			EXPECT_TRUE(View);
			if (View) EXPECT_EQ(**View, 42);
			// The producer is closed, so it is no longer a wait-cycle ancestor.
			EXPECT_TRUE(Dependent.Wait(1));
			if (Dependent.IsCompleted()) EXPECT_EQ(Dependent.GetResult(), 43);
			Observed.Trigger();
		});
		Gate.Trigger();
		EXPECT_TRUE(Task.Wait(2));
		EXPECT_TRUE(Observed.WaitFor(2));
		ShutdownTaskScheduler(true);
		bool Inline = false;
		auto Late = Task.Subscribe([&](ETaskState) { EXPECT_EQ(*Task.GetResult(), 42); Inline = true; });
		EXPECT_TRUE(Inline);
		EXPECT_DEATH({
			auto Invalid = std::make_shared<Private::FTaskBeforeCompletionHook>();
			Invalid->Function = [](ETaskState) {};
			Private::FTaskRuntimeAccess::BindBeforeCompletion(Task, std::move(Invalid));
		}, "must be bound before task completion");
	}

	TEST_F(FTaskCloseTests, PublicObserversDoNotDelayCompletionAndResetDoesNotJoinInflightCalls)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Entered, Release;
		struct FReleaseAndDrain {
			FThreadEvent& Event;
			~FReleaseAndDrain() { Event.Trigger(); ShutdownTaskScheduler(true); }
		} Cleanup{Release};
		Tasks::FTaskEvent Gate("InflightObserverGate", false);
		auto Task = Tasks::LaunchTask("InflightObservedTask", [] { return 42; }, Gate);
		auto Dependent = Tasks::LaunchTask("InflightObserverDependent", [] { return 43; }, Task);
		auto Capture = std::make_shared<int>(1);
		std::weak_ptr<int> Weak = Capture;
		auto Subscription = Task.Subscribe([&, Capture = std::move(Capture), Task](ETaskState) {
			EXPECT_EQ(Task.GetResult(), 42);
			Entered.Trigger();
			Release.Wait();
		});
		Gate.Trigger();
		EXPECT_TRUE(Entered.WaitFor(2));
		EXPECT_TRUE(Task.Wait(0));
		EXPECT_TRUE(Dependent.Wait(2));
		Subscription.Reset();
		EXPECT_FALSE(Weak.expired());
		Release.Trigger();
		ShutdownTaskScheduler(true);
		EXPECT_TRUE(Weak.expired());
	}

	TEST_F(FTaskCloseTests, PublicCompletionObserversRegisterDuringPreCompletionWithoutRunningEarly)
	{
		Tasks::FTaskEvent Event("PreCompletionObserverRace", false);
		FThreadEvent Entered, Release;
		auto Hook = std::make_shared<Private::FTaskBeforeCompletionHook>();
		Hook->Function = [&](ETaskState) { Entered.Trigger(); Release.Wait(); };
		Private::FTaskRuntimeAccess::BindBeforeCompletion(Event, std::move(Hook));
		std::thread Publisher([&] { Event.Trigger(); });
		EXPECT_TRUE(Entered.WaitFor(2));
		std::atomic<uint32> Calls = 0;
		auto Subscription = Tasks::SubscribeTask(Event, [&](ETaskState) {
			EXPECT_TRUE(Event.IsCompleted()); EXPECT_TRUE(Event.Wait()); ++Calls;
		});
		EXPECT_EQ(Calls.load(), 0u);
		Release.Trigger();
		Publisher.join();
		EXPECT_EQ(Calls.load(), 1u);
	}

	TEST_F(FTaskCloseTests, LatePublicRegistrationDuringPinRetirementWaitsForDependencyRelease)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskEvent Event("ObserverPinRetirement", false), ChildGate("ObserverChildGate", false);
		FThreadEvent Started, Release;
		Tasks::FTaskSubscription Late;
		Tasks::TTask<int> Dependent;
		uint32 Calls = 0;
		struct FCleanup {
			Tasks::FTaskEvent& Event;
			Tasks::FTaskEvent& ChildGate;
			FThreadEvent& Release;
			~FCleanup() { Event.Trigger(); ChildGate.Trigger(); Release.Trigger(); ShutdownTaskScheduler(true); }
		} Cleanup{Event, ChildGate, Release};
		auto Blocker = Tasks::LaunchTask("ObserverOccupiedWorker", [&] { Started.Trigger(); Release.Wait(); });
		ASSERT_TRUE(Started.WaitFor(2));
		auto Payload = std::shared_ptr<int>(new int(42), [&](int* Value) {
			delete Value;
			EXPECT_TRUE(Event.IsCompleted());
			Late = Tasks::SubscribeTask(Event, [&](ETaskState) {
				EXPECT_TRUE(Dependent.Wait(1));
				++Calls;
			});
		});
		auto Child = Tasks::LaunchTask("ObserverPinnedChild", [Payload = std::move(Payload)] { return Payload; }, ChildGate);
		Event.AddPrerequisites(Child);
		ChildGate.Trigger();
		ASSERT_TRUE(Child.Wait(2));
		Child = {};
		Dependent = Tasks::LaunchTask("ObserverReleasedDependent", [] { return 42; }, Event);
		Event.Trigger();
		EXPECT_EQ(Calls, 1u);
		EXPECT_EQ(Dependent.GetResult(), 42);
		Release.Trigger();
		EXPECT_TRUE(Blocker.Wait());
	}

	TEST_F(FTaskCloseTests, PublicCompletionObservationRacesPublicationAndSupportsReentrantRegistration)
	{
		for (uint32 Iteration = 0; Iteration < 128; ++Iteration)
		{
			Tasks::FTaskEvent Event("CompletionObservationRace", false);
			FThreadEvent Start;
			std::atomic<uint32> Calls = 0;
			Tasks::FTaskSubscription Nested;
			std::thread Publisher([&] { Start.Wait(); Event.Trigger(); });
			Start.Trigger();
			auto Subscription = Tasks::SubscribeTask(Event, [&](ETaskState) {
				EXPECT_TRUE(Event.IsCompleted());
				Nested = Tasks::SubscribeTask(Event, [&](ETaskState) { EXPECT_TRUE(Event.Wait()); ++Calls; });
				++Calls;
			});
			Publisher.join();
			EXPECT_EQ(Calls.load(), 2u);
		}
	}

	TEST_F(FTaskCloseTests, PublicObserversPreserveOutcomesAndContainExceptions)
	{
		for (auto Outcome : {ETaskState::Succeeded, ETaskState::Failed, ETaskState::Canceled})
		{
			ASSERT_TRUE(InitializeTaskScheduler(1));
			Tasks::FTaskEvent Gate("ObserverOutcomeGate", false);
			auto Task = Tasks::LaunchTask("ObservedOutcome", [Outcome] {
				if (Outcome == ETaskState::Failed) throw std::runtime_error("producer failure");
				if (Outcome == ETaskState::Canceled) Private::FTaskRuntimeAccess::SetCurrentCanceledOutcome();
				return 42;
			}, Gate);
			FThreadEvent Observed;
			auto Subscription = Task.Subscribe([&, Task, Outcome](ETaskState State) {
				EXPECT_TRUE(Task.IsCompleted()); EXPECT_TRUE(Task.Wait());
				EXPECT_EQ(State, Outcome); EXPECT_EQ(Task.GetState(), Outcome);
				if (Outcome == ETaskState::Succeeded) EXPECT_EQ(Task.GetResult(), 42);
				else EXPECT_FALSE(Task.GetResultShared());
				Observed.Trigger();
				throw std::runtime_error("observer failure");
			});
			Gate.Trigger();
			EXPECT_TRUE(Task.Wait(2)); EXPECT_TRUE(Observed.WaitFor(2));
			ShutdownTaskScheduler(true);
			EXPECT_EQ(Task.GetState(), Outcome);
		}
	}

	TEST_F(FTaskCloseTests, DetachingPublicObservationRetiresCapturesWithoutCancelingTheProducer)
	{
		Tasks::FTaskEvent Event("DetachedCompletionObserver", false);
		auto Capture = std::make_shared<int>(1);
		std::weak_ptr<int> Weak = Capture;
		uint32 Calls = 0;
		auto Subscription = Tasks::SubscribeTask(Event, [Capture = std::move(Capture), &Calls](ETaskState) { ++Calls; });
		Subscription.Reset();
		EXPECT_TRUE(Weak.expired());
		EXPECT_FALSE(Event.IsCancellationRequested());
		Event.Trigger();
		EXPECT_EQ(Calls, 0u);
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
				ASSERT_EQ(Child.WaitDetailed().TaskState, ETaskState::Succeeded);
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
				EXPECT_EQ(Target.WaitDetailed(0.005).WaitStatus, ETaskWaitStatus::TimedOut);
				Waiting.Trigger();
				EXPECT_EQ(Target.WaitDetailed(2).TaskState, ETaskState::Succeeded);
			});
			EXPECT_TRUE(Waiting.WaitFor(1));
			std::atomic<bool> UnrelatedRan = false;
			auto Unrelated = Tasks::LaunchTask("UnrelatedReadyNode", [&] { UnrelatedRan = true; });
			EXPECT_FALSE(UnrelatedRan.load());
			Gate.Trigger();
			EXPECT_EQ(Waiter.WaitDetailed(3).TaskState, ETaskState::Succeeded);
			EXPECT_TRUE(Child.IsCompleted());
			EXPECT_EQ(Unrelated.WaitDetailed(1).TaskState, ETaskState::Succeeded);
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
					auto Hook = std::make_shared<Private::FTaskBeforeCompletionHook>();
					Hook->Function = [Next = Events[Index + 1]](ETaskState) mutable { Next.Trigger(); };
					Private::FTaskRuntimeAccess::BindBeforeCompletion(Events[Index], std::move(Hook));
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
