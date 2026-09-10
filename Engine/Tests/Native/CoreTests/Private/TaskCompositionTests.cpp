#include <gtest/gtest.h>
#include "HAL/PlatformLTS.h"
#include "CoreGlobals.h"
#include "Threading/TaskComposition.h"
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
			~FEngineThreadPoolTestGuard() { ShutdownTaskScheduler(false); }
		};
	}
	// Bounded observation for Worker-only fixtures; it never pumps owner callbacks.
	static auto WaitForTaskGroupForTest(const Tasks::FTaskGroup& Group, double Seconds) -> ETaskScopeWaitResult
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(Seconds);
		while (!Group.JoinAsync().IsReady() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		return Group.JoinAsync().IsReady() ? ETaskScopeWaitResult::Quiescent : ETaskScopeWaitResult::TimedOut;
	}

	static_assert(!std::is_default_constructible_v<Tasks::TTaskAdmission<FTaskHandle>>);
	static_assert(!std::is_copy_constructible_v<Tasks::TTaskAdmission<std::unique_ptr<int>>>);
	static_assert(std::is_same_v<Tasks::FTask, Tasks::TTask<void>>);
	static_assert(std::is_move_constructible_v<Tasks::FTask>);
	static_assert(!std::is_copy_constructible_v<Tasks::FTask>);

	TEST(FTaskCompositionTests, SchedulerCompletionSourceSignalsDuringDrainAndCancel)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		for (auto Mode : {ETaskShutdownMode::Drain, ETaskShutdownMode::Cancel})
		{
			ASSERT_TRUE(InitializeTaskScheduler(1));
			ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
			auto Source = Tasks::TCompletionSource<void>::Create();
			int Calls = 0;
			auto Handoff = Tasks::Then(Source.TakeTask(), Tasks::ETaskExecutor::GameThreadDeferred,
				{}, [&] { ++Calls; });
			std::thread Producer([Source] {
				while (IsTaskSchedulerRunning()) std::this_thread::yield();
				EXPECT_TRUE(Source.TrySetValue());
				EXPECT_FALSE(Source.TrySetValue());
			});
			ShutdownTaskSystem(Mode);
			Producer.join();
			EXPECT_TRUE(Handoff.IsCompleted());
			EXPECT_EQ(Calls, Mode == ETaskShutdownMode::Drain ? 1 : 0);
			EXPECT_EQ(Handoff.GetState(), Mode == ETaskShutdownMode::Drain
				? ETaskState::Succeeded : ETaskState::Canceled);
		}
	}

	TEST(FTaskCompositionTests, ResultAccessWaitsAndPreservesUniqueOwnershipUntilTaken)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::TTask<int> Invalid;
		EXPECT_FALSE(Invalid.IsValid());
		EXPECT_FALSE(Invalid.IsCompleted());
		EXPECT_EQ(ETaskWaitStatus::InvalidTask, Invalid.Wait().WaitStatus);
		Tasks::FTaskGroup Group;
		Tasks::FTaskExecutionOptions Options;
		auto Admission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, Options,
			[] { return std::make_unique<int>(42); });
		auto Task = std::move(Admission);
		EXPECT_EQ(42, *Task.GetResult());
		EXPECT_TRUE(Task.IsCompleted());
		EXPECT_EQ(ETaskState::Succeeded, Task.GetState());
		EXPECT_EQ(Task.GetResult().get(), Task.GetResult().get());
		auto Value = std::move(Task).TakeResult();
		EXPECT_EQ(42, *Value);
		EXPECT_FALSE(Task.IsValid());
		auto VoidAdmission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
		Tasks::FTask VoidTask = std::move(VoidAdmission);
		VoidTask.GetResult();
		EXPECT_TRUE(VoidTask.IsValid());
		std::move(VoidTask).TakeResult();
		EXPECT_FALSE(VoidTask.IsValid());
		auto EmptyValue = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] { return std::monostate{}; });
		EXPECT_EQ(std::monostate{}, std::move(EmptyValue).TakeResult());
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}

	TEST(FTaskCompositionTests, CompletedObserversPreserveMoveOnlyCapturesAndFailures)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		Tasks::FTaskExecutionOptions Options;
		for (int Kind = 0; Kind < 3; ++Kind)
		{
			auto Admission = Tasks::TCompletionSource<std::unique_ptr<int>>::Create(Group, Options);
			auto Source = std::move(Admission);
			auto SharedAdmission = Tasks::Share(Source.TakeTask());
			auto Input = std::move(SharedAdmission);
			auto EdgeAdmission = Tasks::ThenCompleted(Input, Tasks::ETaskExecutor::Worker, Options,
				[Capture = std::make_unique<int>(5), Kind](const Tasks::TSharedTask<std::unique_ptr<int>>& Task) {
					EXPECT_EQ(Kind == 0 ? ETaskState::Succeeded : Kind == 1 ? ETaskState::Failed : ETaskState::Canceled, Task.GetState());
					if (Kind == 0) return *Task.GetResult() + *Capture;
					if (Kind == 1)
					{
						const auto& Failure = Task.GetFailure();
						EXPECT_EQ(Tasks::ETaskFailureCode::DependencyBindingFailed, Failure.Code);
						EXPECT_EQ(123u, Failure.RelatedTaskId);
					}
					return *Capture;
				});
			EXPECT_TRUE(Input.IsValid());
			auto Edge = std::move(EdgeAdmission);
			if (Kind == 0) EXPECT_TRUE(Source.TrySetValue(std::make_unique<int>(7)));
			else if (Kind == 1) EXPECT_TRUE(Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, {}, Tasks::ETaskFailureCode::DependencyBindingFailed, 123}));
			else EXPECT_TRUE(Source.TrySetCanceled());
			ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Edge.GetCompletion()).TaskState);
			EXPECT_EQ(Kind == 0 ? 12 : 5, std::move(Edge).TakeResult());
		}
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}

	TEST(FTaskCompositionTests, CompletedObserversFanOutWithoutLosingFailureIdentity)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		Tasks::FTaskExecutionOptions Options;
		for (int Kind = 0; Kind < 3; ++Kind)
		{
			auto Admission = Tasks::TCompletionSource<std::unique_ptr<int>>::Create(Group, Options);
			auto Source = std::move(Admission);
			auto SharedAdmission = Tasks::Share(Source.TakeTask());
			auto Shared = std::move(SharedAdmission);
			auto Callback = [Kind](const Tasks::TSharedTask<std::unique_ptr<int>>& Task) {
				EXPECT_EQ(Kind == 0 ? ETaskState::Succeeded : Kind == 1 ? ETaskState::Failed : ETaskState::Canceled, Task.GetState());
				if (Kind == 0) return *Task.GetResult();
				if (Kind == 1) EXPECT_EQ(321u, Task.GetFailure().RelatedTaskId);
				return -Kind;
			};
			EXPECT_DEATH({ (void)Tasks::ThenCompleted(Shared, static_cast<Tasks::ETaskExecutor>(255), {}, Callback); }, "");
			auto First = Tasks::ThenCompleted(Shared, Tasks::ETaskExecutor::Worker, {}, Callback);
			auto Second = Tasks::ThenCompleted(Shared, Tasks::ETaskExecutor::Worker, {}, Callback);
			if (Kind == 0) Source.TrySetValue(std::make_unique<int>(9));
			else if (Kind == 1) Source.TrySetFailure({ETaskTerminalReason::CallbackFailure, {}, Tasks::ETaskFailureCode::DependencyBindingFailed, 321});
			else Source.TrySetCanceled();
			auto A = std::move(First);
			auto B = std::move(Second);
			ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(A.GetCompletion()).TaskState);
			ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(B.GetCompletion()).TaskState);
			EXPECT_EQ(Kind == 0 ? 9 : -Kind, std::move(A).TakeResult());
			EXPECT_EQ(Kind == 0 ? 9 : -Kind, std::move(B).TakeResult());
			EXPECT_EQ(Kind == 0 ? ETaskState::Succeeded : Kind == 1 ? ETaskState::Failed : ETaskState::Canceled, Shared.GetState());
		}
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}


	TEST(FTaskCompositionTests, CompletedEdgeCancellationDoesNotRunRecovery)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto Admission = Tasks::TCompletionSource<int>::Create(Group, {});
		auto Source = std::move(Admission);
		auto SharedAdmission = Tasks::Share(Source.TakeTask());
		auto Input = std::move(SharedAdmission);
		std::atomic<int> Calls = 0;
		auto EdgeAdmission = Tasks::ThenCompleted(Input, Tasks::ETaskExecutor::Worker, {},
			[&](const Tasks::TSharedTask<int>&) { ++Calls; return 1; });
		auto Edge = std::move(EdgeAdmission);
		Tasks::Cancel(Edge.GetCompletion());
		EXPECT_TRUE(Source.TrySetFailure({}));
		EXPECT_EQ(ETaskState::Canceled, Tasks::Wait(Edge.GetCompletion()).TaskState);
		EXPECT_EQ(0, Calls.load());
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}

	TEST(FTaskCompositionTests, UniqueTransformsVoidAndShare)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		Tasks::FTaskExecutionOptions Options;
		auto RootAdmission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, Options,
			[Capture = std::make_unique<int>(13)](Tasks::FTaskContext& Context) mutable {
				EXPECT_FALSE(Context.GetCancellationToken().IsCancellationRequested());
				return std::move(Capture);
			});
		auto Root = std::move(RootAdmission);
		auto EdgeAdmission = Tasks::Then(std::move(Root), Tasks::ETaskExecutor::Worker, Options,
			[](std::unique_ptr<int>&& Value) { return *Value + 2; });
		EXPECT_FALSE(Root.IsValid());
		auto Edge = std::move(EdgeAdmission);
		auto VoidAdmission = Tasks::Then(std::move(Edge), Tasks::ETaskExecutor::Worker, Options,
			[](int Value) { EXPECT_EQ(15, Value); });
		auto Gate = std::move(VoidAdmission);
		auto TailAdmission = Tasks::Then(std::move(Gate), Tasks::ETaskExecutor::Worker, Options, [] { return 19; });
		auto Tail = std::move(TailAdmission);
		auto SharedAdmission = Tasks::Share(std::move(Tail));
		EXPECT_FALSE(Tail.IsValid());
		auto Shared = std::move(SharedAdmission);
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Shared.GetCompletion()).TaskState);
		auto Value = Shared.GetResultShared();
		ASSERT_TRUE(Value);
		EXPECT_EQ(19, *Value);
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}



	TEST(FTaskCompositionTests, ExternalSourceCancellationWaitsForProducerAndPublishesOnce)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto Admission = Tasks::TCompletionSource<int>::Create(Group, {});
		auto Source = std::move(Admission);
		auto Result = Source.TakeTask();
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::Wait(Result.GetCompletion()).WaitStatus);
		Group.Close(ETaskScopeCloseMode::Cancel);
		EXPECT_FALSE(Result.GetCompletion().IsReady());
		EXPECT_EQ(ETaskScopeWaitResult::TimedOut, WaitForTaskGroupForTest(Group, 0.001));
		EXPECT_TRUE(Source.TrySetValue(42));
		EXPECT_FALSE(Source.TrySetValue(100));
		ASSERT_EQ(ETaskState::Canceled, Tasks::Wait(Result.GetCompletion()).TaskState);
		EXPECT_EQ(ETaskState::Canceled, Result.GetState());
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}

	TEST(FTaskCompositionTests, ExternalSourceAbandonmentAndConcurrentPublication)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		Tasks::TTask<int> Abandoned;
		{
			auto Admission = Tasks::TCompletionSource<int>::Create(Group, {});
			auto Source = std::move(Admission);
			Abandoned = Source.TakeTask();
		}
		ASSERT_EQ(ETaskState::Failed, Tasks::Wait(Abandoned.GetCompletion()).TaskState);
		EXPECT_EQ(Tasks::ETaskFailureCode::AbandonedSource, Abandoned.GetFailure().Code);
		auto Admission = Tasks::TCompletionSource<int>::Create(Group, {});
		auto Source = std::move(Admission);
		auto Result = Source.TakeTask();
		std::atomic<int> Winners = 0;
		std::thread First([&] { if (Source.TrySetValue(7)) ++Winners; });
		std::thread Second([&] { if (Source.TrySetValue(11)) ++Winners; });
		First.join();
		Second.join();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Result.GetCompletion()).TaskState);
		EXPECT_EQ(1, Winners.load());
		const int Value = std::move(Result).TakeResult();
		EXPECT_TRUE(Value == 7 || Value == 11);
		Group.Close();
	}


	TEST(FTaskCompositionTests, DynamicDependenciesRejectCycles)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto AAdmission = Tasks::TCompletionSource<void>::Create(Group, {});
		auto BAdmission = Tasks::TCompletionSource<void>::Create(Group, {});
		auto A = std::move(AAdmission);
		auto B = std::move(BAdmission);
		const auto AHandle = A.GetCompletion().GetTaskHandle();
		const auto BHandle = B.GetCompletion().GetTaskHandle();
		EXPECT_FALSE(Private::FTaskRuntimeAccess::BindDynamicDependency(AHandle, BHandle));
		auto Cycle = Private::FTaskRuntimeAccess::BindDynamicDependency(BHandle, AHandle);
		ASSERT_TRUE(Cycle.has_value());
		EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::DependencyCycle, Cycle->Code);
		EXPECT_TRUE(A.TrySetCanceled());
		EXPECT_TRUE(B.TrySetCanceled());
		Group.Close();
	}

	TEST(FTaskCompositionTests, FanInPreservesDynamicAndHeterogeneousOrder)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		std::vector<Tasks::TTask<int>> Inputs;
		for (int Value : {4, 2, 8})
		{
			auto Admission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [Value] { return Value; });
			Inputs.emplace_back(std::move(Admission));
		}
		EXPECT_DEATH({ (void)Tasks::WhenAll(std::move(Inputs), static_cast<Tasks::ETaskExecutor>(255)); }, "");
		for (auto& Input : Inputs) EXPECT_TRUE(Input.IsValid());
		auto AllAdmission = Tasks::WhenAll(std::move(Inputs));
		auto All = std::move(AllAdmission);
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(All.GetCompletion()).TaskState);
		EXPECT_EQ((std::vector<int>{4, 2, 8}), std::move(All).TakeResult());
		auto EmptyAdmission = Tasks::WhenAll(std::vector<Tasks::TTask<int>>{});
		auto Empty = std::move(EmptyAdmission);
		EXPECT_TRUE(Empty.GetCompletion().IsReady());
		EXPECT_TRUE(std::move(Empty).TakeResult().empty());

		auto A = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] { return std::make_unique<int>(17); });
		auto B = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
		auto TupleAdmission = Tasks::WhenAll(std::make_tuple(std::move(A), std::move(B)));
		auto Tuple = std::move(TupleAdmission);
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Tuple.GetCompletion()).TaskState);
		auto Values = std::move(Tuple).TakeResult();
		EXPECT_EQ(17, *std::get<0>(Values));
		Group.Close();
	}

	TEST(FTaskCompositionTests, FanInFailurePrecedesCancellationAndUsesInputOrder)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto FirstAdmission = Tasks::TCompletionSource<int>::Create(Group, {});
		auto SecondAdmission = Tasks::TCompletionSource<int>::Create(Group, {});
		auto CanceledAdmission = Tasks::TCompletionSource<int>::Create(Group, {});
		auto First = std::move(FirstAdmission);
		auto Second = std::move(SecondAdmission);
		auto Canceled = std::move(CanceledAdmission);
		std::vector<Tasks::TTask<int>> Inputs;
		Inputs.emplace_back(Canceled.TakeTask());
		Inputs.emplace_back(Second.TakeTask());
		Inputs.emplace_back(First.TakeTask());
		auto Admission = Tasks::WhenAll(std::move(Inputs));
		auto Result = std::move(Admission);
		EXPECT_TRUE(Canceled.TrySetCanceled());
		EXPECT_TRUE(First.TrySetFailure({}));
		EXPECT_TRUE(Second.TrySetFailure({}));
		ASSERT_EQ(ETaskState::Failed, Tasks::Wait(Result.GetCompletion()).TaskState);
		const auto Failure = Result.GetFailure();
		EXPECT_EQ(1u, Failure.InputIndex);
		EXPECT_EQ(Second.GetCompletion().GetTaskHandle().GetTaskId(), Failure.RelatedTaskId);
		Group.Close();
	}

	TEST(FTaskCompositionTests, SharedFanInDuplicatesAndAsyncCancellationAreLocal)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		auto SourceAdmission = Tasks::TCompletionSource<int>::Create(Group, {});
		auto Source = std::move(SourceAdmission);
		auto SourceTask = Source.TakeTask();
		auto SharedAdmission = Tasks::Share(std::move(SourceTask));
		auto Shared = std::move(SharedAdmission);
		auto RootAdmission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
		auto Root = std::move(RootAdmission);
		auto ObserverAdmission = Tasks::ThenAsync(std::move(Root), Tasks::ETaskExecutor::Worker, {}, [Shared] { return Shared; });
		auto Observer = std::move(ObserverAdmission);
		EXPECT_TRUE(Tasks::Cancel(Observer.GetCompletion()));
		std::vector<Tasks::TSharedTask<int>> Inputs{Shared, Shared};
		auto AllAdmission = Tasks::WhenAll(Inputs);
		auto All = std::move(AllAdmission);
		EXPECT_TRUE(Source.TrySetValue(27));
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(All.GetCompletion()).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, Shared.GetCompletion().GetState());
		const auto Values = std::move(All).TakeResult();
		ASSERT_EQ(2u, Values.size());
		EXPECT_EQ(Values[0].get(), Values[1].get());
		EXPECT_EQ(27, *Values[1]);
		Group.Close();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_EQ(ETaskState::Canceled, Observer.GetState());
	}

	TEST(FTaskCompositionTests, ThenAsyncCancelsUniqueDeferredInnerWithoutPumping)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTaskGroup Group;
		auto RootAdmission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
		auto Root = std::move(RootAdmission);
		std::atomic<int> Called = 0;
		FThreadEvent Spawned;
		auto Admission = Tasks::ThenAsync(std::move(Root), Tasks::ETaskExecutor::Worker, {}, [&] {
			auto Inner = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::GameThreadDeferred,
				{}, [&] { ++Called; return 12; });
			Spawned.Trigger();
			return Inner;
		});
		auto Outer = std::move(Admission);
		ASSERT_TRUE(Spawned.WaitFor(1.0));
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::Wait(Outer.GetCompletion()).WaitStatus);
		EXPECT_TRUE(Tasks::Cancel(Outer.GetCompletion()));
		Group.Close();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_EQ(ETaskState::Canceled, Outer.GetState());
		EXPECT_EQ(0, Called.load());
	}



	TEST(FTaskCompositionTests, DrainAdmitsOnlyLiveParentChildrenAndJoinIncludesThem)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		FThreadEvent ParentStarted, ContinueParent, ContinueChild;
		std::optional<Tasks::TTask<int>> Child;
		std::unique_ptr<Tasks::FTaskContext> RetainedContext;
		auto Admission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&](Tasks::FTaskContext& Context) {
			RetainedContext = std::make_unique<Tasks::FTaskContext>(Context.GetCancellationToken(), Group.GetToken());
			ParentStarted.Trigger();
			if (!ContinueParent.WaitFor(2.0)) return;
			auto ChildAdmission = Context.LaunchChild(Tasks::ETaskExecutor::Worker, {}, [&] {
				(void)ContinueChild.WaitFor(2.0);
				return 37;
			});
			Child.emplace(std::move(ChildAdmission));
		});
		auto Parent = std::move(Admission);
		ASSERT_TRUE(ParentStarted.WaitFor(1.0));
		Group.Close();
		auto Join = Group.JoinAsync();
		EXPECT_FALSE(Join.IsReady());
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::Wait(Join).WaitStatus);
		EXPECT_DEATH({ (void)Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] {}); }, "");
		ContinueParent.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Parent.GetCompletion()).TaskState);
		ASSERT_TRUE(Child.has_value());
		EXPECT_FALSE(Join.IsReady());
		EXPECT_DEATH({ (void)RetainedContext->LaunchChild(Tasks::ETaskExecutor::Worker, {}, [] {}); }, "");
		ContinueChild.Trigger();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_TRUE(Join.IsReady());
		EXPECT_EQ(37, std::move(*Child).TakeResult());
	}


	TEST(FTaskCompositionTests, AsyncContextSpawnsChildDuringDrain)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		FThreadEvent Started, Continue;
		auto RootAdmission = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] { return 6; });
		auto Root = std::move(RootAdmission);
		auto OuterAdmission = Tasks::ThenAsync(std::move(Root), Tasks::ETaskExecutor::Worker, {},
			[&](Tasks::FTaskContext& Context, int Value) {
				Started.Trigger(); (void)Continue.WaitFor(2.0);
				return Context.LaunchChild(Tasks::ETaskExecutor::Worker, {}, [Value] { return Value + 3; });
			});
		auto Outer = std::move(OuterAdmission);
		ASSERT_TRUE(Started.WaitFor(1.0));
		Group.Close();
		EXPECT_FALSE(Group.JoinAsync().IsReady());
		Continue.Trigger();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_EQ(9, std::move(Outer).TakeResult());
	}


	TEST(FTaskExecutorTests, CpuRootsAndContinuationsHonorPriorityWithBoundedOldestService)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		FThreadEvent Started;
		FThreadEvent Release;
		auto Blocker = Tasks::LaunchTask("PriorityBlocker", [&] { Started.Trigger(); Release.Wait(); }).GetCompletion().GetTaskHandle();
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::vector<int> Order;
		std::vector<FTaskHandle> Completions;
		Tasks::FTaskExecutionOptions Options;
		Options.Priority = ETaskPriority::Low;
		auto Low = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, Options, [&] { Order.push_back(-1); });
		Completions.push_back(std::move(Low).GetCompletion().GetTaskHandle());
		auto SourceAdmission = Tasks::TCompletionSource<void>::Create(Group, {});
		auto Source = std::move(SourceAdmission);
		auto Input = Source.TakeTask();
		Options.Priority = ETaskPriority::High;
		auto Edge = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, Options, [&] { Order.push_back(99); });
		Completions.push_back(std::move(Edge).GetCompletion().GetTaskHandle());
		Source.TrySetValue();
		for (int Index = 0; Index < 24; ++Index)
		{
			auto High = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, Options, [&, Index] { Order.push_back(Index); });
			Completions.push_back(std::move(High).GetCompletion().GetTaskHandle());
		}
		Release.Trigger();
		WaitAll(Completions);
		ASSERT_EQ(26u, Order.size());
		EXPECT_EQ(99, Order.front());
		EXPECT_LE(std::ranges::find(Order, -1) - Order.begin(), 7);
		std::erase(Order, -1);
		for (int Index = 0; Index < 24; ++Index) EXPECT_EQ(Index, Order[Index + 1]);
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}
	TEST(FTaskExecutorTests, BlockingIoChildWaitAndCrossExecutorShutdownDrain)
	{
		FEngineThreadPoolTestGuard Guard;
		for (bool Drain : {true, false})
		{
			ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .NumBlockingIOThreads = 1}));
			Tasks::FTaskGroup Group;
			auto Parent = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::BlockingIO, {}, [](Tasks::FTaskContext& Context) {
				auto Child = Context.LaunchChild(Tasks::ETaskExecutor::BlockingIO, {}, [] { return 11; });

				auto Task = std::move(Child);
				EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(Task.GetCompletion()).TaskState);
			});
			auto ParentTask = std::move(Parent);
			EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(ParentTask.GetCompletion()).TaskState);
			FThreadEvent Started, Release;
			auto IO = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::BlockingIO, {}, [&] { Started.Trigger(); Release.Wait(); return 3; });
			auto Task = std::move(IO);
			auto Edge = Tasks::Then(std::move(Task), Tasks::ETaskExecutor::Worker, {}, [](int Value) { return Value + 1; });
			auto Tail = std::move(Edge);
			ASSERT_TRUE(Started.WaitFor(1.0));
			std::thread Shutdown([&] { ShutdownTaskScheduler(Drain); });
			Release.Trigger();
			Shutdown.join();
			EXPECT_TRUE(Tail.GetCompletion().IsReady());
			if (Drain) EXPECT_EQ(ETaskState::Succeeded, Tail.GetCompletion().GetState());
			EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().BlockingIOReservations);
			EXPECT_TRUE(Group.JoinAsync().IsReady());
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
		auto Blocker = Tasks::LaunchTask("HoldCpu", [&] { CpuBlocked.Trigger(); ReleaseCpu.Wait(); }).GetCompletion().GetTaskHandle();
		ASSERT_TRUE(CpuBlocked.WaitFor(1.0));
		Tasks::FTaskGroup Group;
		auto Parent = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::BlockingIO, {}, [&](Tasks::FTaskContext& Context) {
			auto Child = Context.LaunchChild(Tasks::ETaskExecutor::Worker, {}, [](Tasks::FTaskContext& ChildContext) {
				auto IO = ChildContext.LaunchChild(Tasks::ETaskExecutor::BlockingIO, {}, [] {});
				auto Task = std::move(IO);
				EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(Task.GetCompletion()).TaskState);
			});
			auto Task = std::move(Child);
			EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(Task.GetCompletion()).TaskState);
			Finished.Trigger();
		});
		const bool CompletedWhileCpuBlocked = Finished.WaitFor(1.0);
		Group.Close(CompletedWhileCpuBlocked ? ETaskScopeCloseMode::Drain : ETaskScopeCloseMode::Cancel);
		ReleaseCpu.Trigger();
		EXPECT_TRUE(CompletedWhileCpuBlocked);
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		WaitTask(Blocker);
	}
	TEST(FTaskAcceptanceTests, QueuesAboveFormerDefaultsAndDrainsWithExecutorAffinity)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTaskGroup Group;
		FThreadEvent Started, Release;
		std::atomic<uint32> WorkerCalls = 0, DeferredCalls = 0;
		auto Blocker = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] { Started.Trigger(); (void)Release.WaitFor(20.0); });
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::vector<Tasks::TTask<void>> Workers, Deferred;
		for (uint32 Index = 0; Index < 17000; ++Index)
			Workers.push_back(Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] {
				EXPECT_FALSE(IsInGameThread()); ++WorkerCalls;
			}));
		for (uint32 Index = 0; Index < 1100; ++Index)
			Deferred.push_back(Tasks::LaunchTask(Group, Tasks::ETaskExecutor::GameThreadDeferred, {}, [&] {
				EXPECT_TRUE(IsInGameThread()); ++DeferredCalls;
			}));
		const auto Peak = GetTaskSchedulerDiagnostics();
		EXPECT_GT(Peak.CurrentTaskReservationCount, 16384u);
		EXPECT_EQ(0u, Peak.CapacityRejectedTaskCount);
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
		Group.Close();
		ShutdownTaskSystem(ETaskShutdownMode::Drain);
		EXPECT_EQ(17000u, WorkerCalls.load());
		EXPECT_EQ(1100u, DeferredCalls.load());
		for (auto& Task : Workers) EXPECT_EQ(ETaskState::Succeeded, Task.GetState());
		for (auto& Task : Deferred) EXPECT_EQ(ETaskState::Succeeded, Task.GetState());
		const auto Final = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(0u, Final.CurrentTaskReservationCount);
		for (const auto& Storage : Final.ExecutorStorage) { EXPECT_EQ(0u, Storage.CurrentNodes); EXPECT_EQ(0u, Storage.CurrentBytes); EXPECT_EQ(0u, Storage.CurrentPendingNodes); EXPECT_EQ(0u, Storage.CurrentPendingBytes); EXPECT_EQ(0u, Storage.CurrentRunningBodies); }
	}

	TEST(FTaskAcceptanceTests, SmallThresholdIncludesSourcesCompositionAndIoChildren)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 1,
			.NumBlockingIOThreads = 1, .MaxBlockingIOTasks = 1}));
		Tasks::FTaskGroup Group;
		auto Source = Tasks::TCompletionSource<int>::Create(Group, {});
		auto Shared = Tasks::Share(Source.TakeTask());
		std::vector<Tasks::TTask<int>> Inputs;
		for (int Index = 0; Index < 32; ++Index)
			Inputs.push_back(Tasks::Then(Shared, Tasks::ETaskExecutor::Worker, {}, [Index](int Value) { return Value + Index; }));
		auto All = Tasks::WhenAll(std::move(Inputs));
		auto Io = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::BlockingIO, {}, [](Tasks::FTaskContext& Context) {
			auto Child = Context.LaunchChild(Tasks::ETaskExecutor::BlockingIO, {}, [] { return 9; });
			return std::move(Child).TakeResult();
		});
		Source.TrySetValue(10);
		EXPECT_EQ(9, std::move(Io).TakeResult());
		auto Values = std::move(All).TakeResult();
		ASSERT_EQ(32u, Values.size());
		EXPECT_EQ(41, Values.back());
		Group.Close();
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CapacityRejectedTaskCount);
	}

	TEST(FTaskAcceptanceTests, CancelRetiresQueuedBodiesAndUniqueCapturesOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 1}));
		Tasks::FTaskGroup Group;
		FThreadEvent Started, Release;
		auto Blocker = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] { Started.Trigger(); (void)Release.WaitFor(5.0); });
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::atomic<int> Destroyed = 0, Called = 0;
		struct FPayload { std::atomic<int>* Count; ~FPayload() { ++*Count; } };
		std::vector<Tasks::TTask<void>> Queued;
		for (int Index = 0; Index < 64; ++Index)
			Queued.push_back(Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {},
				[Payload = std::make_unique<FPayload>(&Destroyed), &Called] { ++Called; }));
		Group.Close(ETaskScopeCloseMode::Cancel);
		Release.Trigger();
		ShutdownTaskScheduler(false);
		EXPECT_EQ(0, Called.load());
		EXPECT_EQ(64, Destroyed.load());
		for (auto& Task : Queued) EXPECT_EQ(ETaskState::Canceled, Task.GetState());
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
	}

	TEST(FTaskAcceptanceTests, AllocationFailureIsFatalForOrdinaryConstruction)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		for (int32 Checkpoint = 1; Checkpoint <= 6; ++Checkpoint)
		{
			EXPECT_DEATH({
				Private::SetTaskAdmissionAllocationFailureForTests(Checkpoint);
				(void)Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
			}, "");
		}
		Group.Close();
	}

	TEST(FTaskAcceptanceTests, CheckedAllocationFailureRollsBackScopeAndDependencyReservations)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto Input = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] { return 13; });
		ASSERT_EQ(ETaskState::Succeeded, Input.Wait().TaskState);
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().CurrentTaskReservationCount != 0 && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		for (int32 Checkpoint = 1; Checkpoint <= 5; ++Checkpoint)
		{
			Private::SetTaskAdmissionAllocationFailureForTests(Checkpoint);
			auto Rejected = Private::TryLaunchContinuationTask(Input.GetCompletion().GetTaskHandle(),
				"AllocationRollback", [](const FTaskCancellationToken&) {}, {}, {}, ETaskDependencyKind::Success);
			ASSERT_FALSE(Rejected.HasValue());
			EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::CapacityExhausted, Rejected.GetError().Code);
			EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
			EXPECT_EQ(0u, Group.GetDiagnostics().CurrentActiveCount);
		}
		auto Output = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, {}, [](int Value) { return Value + 1; });
		EXPECT_EQ(14, std::move(Output).TakeResult());
		Group.Close();
	}

	TEST(FTaskCompositionTests, DroppedCanceledAndThrowingUniqueResultsReleaseExactlyOnce)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		struct FValue { std::atomic<int>& Count; ~FValue() { ++Count; } };
		std::atomic<int> Destroyed = 0;
		Tasks::FTaskGroup Group;
		{
			auto Dropped = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] {
				return std::unique_ptr<FValue>(new FValue{Destroyed});
			});
			ASSERT_EQ(ETaskState::Succeeded, Dropped.Wait().TaskState);
		}
		EXPECT_EQ(1, Destroyed.load());
		{
			auto Input = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] {
				return std::unique_ptr<FValue>(new FValue{Destroyed});
			});
			auto Sink = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, {},
				[](std::unique_ptr<FValue>) { throw std::runtime_error("unique sink failed"); });
			ASSERT_EQ(ETaskState::Failed, Sink.Wait().TaskState);
			EXPECT_EQ(2, Destroyed.load());
		}
		{
			FThreadEvent Started, Release;
			auto Running = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [&] {
				auto Value = std::unique_ptr<FValue>(new FValue{Destroyed});
				Started.Trigger(); (void)Release.WaitFor(2.0); return Value;
			});
			EXPECT_TRUE(Started.WaitFor(1.0));
			Tasks::Cancel(Running.GetCompletion()); Release.Trigger();
			EXPECT_EQ(ETaskState::Canceled, Running.Wait().TaskState);
		}
		EXPECT_EQ(3, Destroyed.load());
	}

	TEST(FTaskCompositionTests, CompletionRegistrationRacesTerminalPublication)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		std::atomic<int> Calls = 0;
		for (int Index = 0; Index < 128; ++Index)
		{
			auto Input = Tasks::Share(Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] { return 42; }));
			auto Edge = Tasks::ThenCompleted(Input, Tasks::ETaskExecutor::Worker, {},
				[&](const Tasks::TSharedTask<int>& Completed) { EXPECT_EQ(42, Completed.GetResult()); ++Calls; });
			ASSERT_EQ(ETaskState::Succeeded, Edge.Wait().TaskState);
		}
		EXPECT_EQ(128, Calls.load());
	}

	TEST(FTaskCompositionTests, RootGenerationAndFanInExtraPrerequisitesArePreserved)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTaskGroup Group;
		FTaskGenerationSource Generation;
		Tasks::FTaskExecutionOptions Options;
		Options.GenerationToken = Generation.Capture();
		auto Stale = Tasks::LaunchTask(Group, Tasks::ETaskExecutor::GameThreadDeferred, Options, [] { ADD_FAILURE(); });
		Generation.Advance();
		PumpGameThreadDeferredWork();
		EXPECT_EQ(ETaskState::Canceled, Stale.GetState());
		auto Source = Tasks::TCompletionSource<void>::Create(Group, {});
		const std::array Dependencies{Source.TakeTask().GetCompletion().GetTaskHandle()};
		Options = {}; Options.Prerequisites = Dependencies;
		std::vector<Tasks::TTask<int>> Inputs;
		Inputs.push_back(Tasks::LaunchTask(Group, Tasks::ETaskExecutor::Worker, {}, [] { return 7; }));
		auto All = Tasks::WhenAll(std::move(Inputs), Tasks::ETaskExecutor::Worker, Options);
		EXPECT_FALSE(All.IsCompleted());
		Source.TrySetValue();
		ASSERT_EQ(ETaskState::Succeeded, All.Wait().TaskState);
		EXPECT_EQ(7, std::move(All).TakeResult()[0]);
	}

	TEST(FTaskAcceptanceTests, CheckedParallelForPartialSubmissionDrainsAcceptedCaptures)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 2, .MaxNonterminalTasks = 1}));
		std::atomic<int> Active = 0, Destroyed = 0;
		struct FCapture { std::atomic<int>& Count; ~FCapture() { ++Count; } };
		const auto Result = ParallelForCancelable("PartialCheckedChunks", 3,
			[Capture = std::shared_ptr<FCapture>(new FCapture{Destroyed}), &Active]
			(uint64, const FParallelForCancellationToken& Token) {
				++Active;
				const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (!Token.IsCancellationRequested() && std::chrono::steady_clock::now() < Deadline)
					std::this_thread::yield();
				--Active;
			}, {.MinBatchSize = 1});
		EXPECT_EQ(ETaskState::Canceled, Result.State);
		EXPECT_EQ(0, Active.load());
		EXPECT_EQ(1, Destroyed.load());
		const auto Diagnostics = GetTaskSchedulerDiagnostics();
		EXPECT_EQ(1u, Diagnostics.CompletedTaskCount);
		EXPECT_EQ(1u, Diagnostics.CapacityRejectedTaskCount);
		EXPECT_EQ(0u, Diagnostics.CurrentTaskReservationCount);
	}

}
