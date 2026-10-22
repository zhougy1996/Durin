#include <gtest/gtest.h>
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Threading/ThreadEvent.h"
#include "Threading/TaskComposition.h"

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

	TEST_F(FTaskCloseTests, DeepAsyncResultsPreserveAliasesAndRetireOwnerLifetimes)
	{
		ASSERT_TRUE(InitializeTaskScheduler(2));
		constexpr uint32 Depth = 8192;
		std::atomic<uint32> Retired = 0;
		struct FOwner
		{
			std::atomic<uint32>& Count;
			~FOwner() { ++Count; }
		};
		auto Seed = Tasks::LaunchTask("ForwardSeed", [] {});
		Seed.Wait();
		auto Root = Tasks::LaunchTask("ForwardValue", [] { return std::make_unique<int>(42); });
		Root.Wait();
		auto* Address = &Root.GetResult();
		auto Current = Root;
		std::thread Producer([&] {
			for (uint32 Index = 0; Index < Depth; ++Index)
			{
				auto Next = Tasks::ThenAsync(Seed, Tasks::ETaskExecutor::Worker, {}, [Current] { return Current; });
				ASSERT_EQ(Next.Wait().TaskState, ETaskState::Succeeded);
				Next = Next.WithLifetime(std::shared_ptr<FOwner>(new FOwner{Retired}));
				Current = std::move(Next);
			}
		});
		Producer.join();
		ShutdownTaskScheduler(false);
		EXPECT_EQ(&Current.GetResult(), Address);
		*Current.GetResult() = 73;
		EXPECT_EQ(*Root.GetResult(), 73);
		EXPECT_EQ(Retired.load(), 0u);
		Root = {};
		auto View = Current.GetResultShared();
		Current = {};
		EXPECT_EQ(**View, 73);
		EXPECT_EQ(Retired.load(), 0u);
		View.reset();
		EXPECT_EQ(Retired.load(), Depth);
	}

	TEST_F(FTaskCloseTests, VoidStoragePublishesAndDiscardsInlineValue)
	{
		for (auto Outcome : {ETaskState::Succeeded, ETaskState::Failed, ETaskState::Canceled})
		{
			auto Storage = Private::MakeVoidTaskResultState();
			EXPECT_EQ(Storage->PeekPublished(), nullptr);
			Storage->SetPending(std::monostate{});
			EXPECT_EQ(Storage->PeekPublished(), nullptr);
			Storage->Complete(Outcome);
			EXPECT_EQ(Storage->PeekPublished() != nullptr, Outcome == ETaskState::Succeeded);
			Storage->Discard();
			EXPECT_EQ(Storage->PeekPublished(), nullptr);
		}
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

	TEST_F(FTaskCloseTests, PreparedDependenciesRaceClosureWithoutLeakingReservations)
	{
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 2, .MaxNonterminalTasks = 32}));
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
					auto Admission = Private::TryLaunchCancelableTaskWithCompletion("PreparedClose",
						[&](const FTaskCancellationToken&) { ++Runs; }, {}, Options);
					if (Admission.HasValue()) ++Accepted;
					else EXPECT_TRUE(Admission.GetError().Code == Tasks::ETaskAdmissionErrorCode::LifetimeClosed
						|| Admission.GetError().Code == Tasks::ETaskAdmissionErrorCode::CapacityExhausted);
				}
			});
		while (Ready.load() != Producers.size()) std::this_thread::yield();
		Start.Trigger();
		Gate.Trigger();
		ShutdownTaskScheduler(true);
		for (auto& Producer : Producers) Producer.join();
		EXPECT_EQ(Runs.load(), Accepted.load());
		EXPECT_EQ(GetTaskSchedulerDiagnostics().CurrentTaskReservationCount, 0u);
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
			auto Next = Tasks::Then(Task, Tasks::ETaskExecutor::Worker, {}, [](const FTaskCancellationToken& Token, auto& Value) {
				EXPECT_FALSE(Token.IsCancellationRequested());
				return *Value + 1;
			});
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
		EXPECT_EQ(GetTaskSchedulerDiagnostics().CurrentTaskReservationCount, 0u);
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

	TEST_F(FTaskCloseTests, MixedDependenciesRetainDynamicResultsAndNotifyExecutionWaiters)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		struct FCase { bool bNested; bool bDynamicFirst; };
		for (const auto [Nested, DynamicFirst] : {FCase{false, false}, FCase{true, false}, FCase{true, true}})
		{
			auto Source = Tasks::TCompletionSource<void>::Create();
			Tasks::FTask Outer = Source.GetTaskHandle();
			Tasks::FTaskEvent Gate("MixedDependencyGate", false);
			std::weak_ptr<int> Weak;
			{
				auto InnerSource = Tasks::TCompletionSource<std::shared_ptr<int>>::Create();
				auto Inner = InnerSource.TakeTask();
				if (Nested && !DynamicFirst) ASSERT_FALSE(Private::FTaskRuntimeAccess::AddCompletionDependency(Outer, Gate));
				ASSERT_FALSE(Private::FTaskRuntimeAccess::BindDynamicDependency(Outer, Inner));
				if (Nested && DynamicFirst) ASSERT_FALSE(Private::FTaskRuntimeAccess::AddCompletionDependency(Outer, Gate));
				auto Value = std::make_shared<int>(42);
				Weak = Value;
				ASSERT_TRUE(InnerSource.TrySetValue(std::move(Value)));
			}
			auto Waiter = Tasks::LaunchTask("MixedDependencyWaiter", [&] {
				EXPECT_TRUE(Weak.expired());
				return 42;
			}, Tasks::Prerequisites(Outer, Gate));
			EXPECT_FALSE(Outer.TryRetractAndExecute());
			EXPECT_FALSE(Outer.TryRetractAndExecute());
			EXPECT_FALSE(Weak.expired());
			ASSERT_TRUE(Source.TrySetValue());
			EXPECT_EQ(Outer.IsCompleted(), !Nested);
			EXPECT_EQ(Weak.expired(), !Nested);
			Gate.Trigger();
			EXPECT_EQ(Waiter.GetResult(), 42);
			EXPECT_TRUE(Outer.IsCompleted());
			EXPECT_TRUE(Weak.expired());
		}
	}

	TEST_F(FTaskCloseTests, RejectedConstructedNodesRetirePayloadOutsideSchedulerLocks)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (int32 Checkpoint = 1; Checkpoint <= 3; ++Checkpoint)
		{
			uint32 Destroyed = 0, Calls = 0;
			struct FCapture
			{
				uint32& Destroyed;
				~FCapture()
				{
					// Reentering scheduler diagnostics must be safe even when a fully
					// constructed node is rolled back after registry insertion.
					EXPECT_EQ(GetTaskSchedulerDiagnostics().CurrentTaskReservationCount, 0u);
					++Destroyed;
				}
			};
			auto Capture = std::shared_ptr<FCapture>(new FCapture{Destroyed});
			Private::FMoveOnlyTaskFunction Body = [Capture, &Calls](const FTaskCancellationToken&) { ++Calls; };
			Private::FTaskCompletionFunction Completion = [Capture, &Calls](ETaskState) { ++Calls; };
			Private::FTaskResultStorage Result{Capture, true};
			Capture.reset();
			FTaskCancellationSource Cancellation;
			FTaskLaunchOptions Options;
			Options.CancellationToken = Cancellation.GetToken();
			Private::SetTaskAdmissionAllocationFailureForTests(Checkpoint);
			auto Rejected = Private::TryLaunchCancelableTaskWithCompletion("RejectedNodePayload",
				std::move(Body), std::move(Completion), Options, 0, std::move(Result));
			ASSERT_FALSE(Rejected.HasValue());
			EXPECT_EQ(Rejected.GetError().Code, Tasks::ETaskAdmissionErrorCode::CapacityExhausted);
			Cancellation.RequestCancellation();
			EXPECT_EQ(Calls, 0u);
			EXPECT_EQ(Destroyed, 1u);
			EXPECT_EQ(GetTaskSchedulerDiagnostics().NonterminalTaskCount, 0u);
		}
	}

	TEST_F(FTaskCloseTests, NodeNotificationsRevisitBlockedNestedAndDynamicGraphs)
	{
		ASSERT_TRUE(InitializeTaskScheduler(1));
		for (bool Dynamic : {false, true})
		{
			Tasks::FTaskEvent Gate("ReadinessGate", false);
			Tasks::FTaskEvent Event("NestedWaitTarget", false);
			auto Source = Tasks::Detail::FCompletionSourceAccess::Create<void>({}, false);
			auto Child = Tasks::LaunchTask("LateReadyChild", [&] { Source.TrySetValue(); }, Tasks::Prerequisites(Gate));
			Tasks::FTask Target;
			if (Dynamic)
			{
				Target = Source.GetTaskHandle();
				ASSERT_FALSE(Private::FTaskRuntimeAccess::BindDynamicDependency(Target, Child));
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
