#include <gtest/gtest.h>
#include "HAL/PlatformLTS.h"
#include "CoreGlobals.h"
#include "Threading/TaskOperation.h"
#include "Threading/ThreadEvent.h"
#include "Threading/RunnableThread.h"

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

	TEST(FTaskCompositionTests, UniqueTransformsVoidAndShare)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		Tasks::FTaskExecutionOptions Options;
		Options.EstimatedResultBytes = 32;
		auto RootAdmission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options,
			[Capture = std::make_unique<int>(13)](Tasks::FTaskContext& Context) mutable {
				EXPECT_FALSE(Context.GetCancellationToken().IsCancellationRequested());
				return std::move(Capture);
			});
		ASSERT_TRUE(RootAdmission.HasValue());
		auto Root = std::move(RootAdmission).TakeValue();
		auto EdgeAdmission = Tasks::Then(std::move(Root), Tasks::ETaskExecutor::Worker, Options,
			[](std::unique_ptr<int>&& Value) { return *Value + 2; });
		ASSERT_TRUE(EdgeAdmission.HasValue());
		EXPECT_FALSE(Root.IsValid());
		auto Edge = std::move(EdgeAdmission).TakeValue();
		auto VoidAdmission = Tasks::Then(std::move(Edge), Tasks::ETaskExecutor::Worker, Options,
			[](int Value) { EXPECT_EQ(15, Value); });
		ASSERT_TRUE(VoidAdmission.HasValue());
		auto Gate = std::move(VoidAdmission).TakeValue();
		auto TailAdmission = Tasks::Then(std::move(Gate), Tasks::ETaskExecutor::Worker, Options, [] { return 19; });
		ASSERT_TRUE(TailAdmission.HasValue());
		auto Tail = std::move(TailAdmission).TakeValue();
		auto SharedAdmission = Tasks::Share(std::move(Tail));
		ASSERT_TRUE(SharedAdmission.HasValue());
		EXPECT_FALSE(Tail.IsValid());
		auto Shared = std::move(SharedAdmission).TakeValue();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Shared.GetCompletion()).TaskState);
		auto Value = Shared.GetResultShared();
		ASSERT_TRUE(Value);
		EXPECT_EQ(19, *Value);
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}

	TEST(FTaskCompositionTests, RejectedEdgesPreserveInputAndDeclarePayload)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor({.MaxPayloadBytesPerEntry = 64}));
		Tasks::FTaskGroup Group;
		Tasks::FTaskExecutionOptions Options;
		auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options, [] { return 23; });
		ASSERT_TRUE(Admission.HasValue());
		auto Task = std::move(Admission).TakeValue();
		auto Rejected = Tasks::Then(std::move(Task), static_cast<Tasks::ETaskExecutor>(255), Options, [](int Value) { return Value; });
		ASSERT_FALSE(Rejected.HasValue());
		EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::UnsupportedExecutor, Rejected.GetError().Code);
		EXPECT_TRUE(Task.IsValid());
		Options.EstimatedCaptureBytes = 65;
		auto Oversize = Tasks::Then(std::move(Task), Tasks::ETaskExecutor::GameThreadDeferred, Options, [](int Value) { return Value; });
		ASSERT_FALSE(Oversize.HasValue());
		EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::InvalidPayloadDeclaration, Oversize.GetError().Code);
		EXPECT_TRUE(Task.IsValid());
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Task.GetCompletion()).TaskState);
		auto Outcome = std::move(Task).TakeOutcome();
		ASSERT_TRUE(std::holds_alternative<int>(Outcome));
		EXPECT_EQ(23, std::get<int>(Outcome));
		Group.Close();
	}

	TEST(FTaskCompositionTests, CallableAllocationFailureRollsBackUniqueClaim)
	{
		// Simulates failure while constructing the erased continuation, after claim reservation.
		struct FFailingMove
		{
			FFailingMove() = default;
			FFailingMove(FFailingMove&&) { throw std::bad_alloc(); }
			auto operator()(int Value) -> int { return Value; }
		};
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		Tasks::FTaskExecutionOptions Options;
		auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options, [] { return 31; });
		ASSERT_TRUE(Admission.HasValue());
		auto Input = std::move(Admission).TakeValue();
		auto Failed = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, Options, FFailingMove{});
		ASSERT_FALSE(Failed.HasValue());
		EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::CapacityExhausted, Failed.GetError().Code);
		ASSERT_TRUE(Input.IsValid());
		auto Retried = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, Options, [](int Value) { return Value + 1; });
		ASSERT_TRUE(Retried.HasValue());
		auto Result = std::move(Retried).TakeValue();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Result.GetCompletion()).TaskState);
		EXPECT_EQ(32, std::get<int>(std::move(Result).TakeOutcome()));
		Group.Close();
		auto Closed = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options, [] {});
		ASSERT_FALSE(Closed.HasValue());
		EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::GroupClosed, Closed.GetError().Code);
	}

	TEST(FTaskCompositionTests, ExternalSourceCancellationWaitsForProducerAndPublishesOnce)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto Admission = Tasks::TCompletionSource<int>::TryCreate(Group, {});
		ASSERT_TRUE(Admission.HasValue());
		auto Source = std::move(Admission).TakeValue();
		auto Result = Source.TakeTask();
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::Wait(Result.GetCompletion()).WaitStatus);
		Group.Close(ETaskScopeCloseMode::Cancel);
		EXPECT_FALSE(Result.GetCompletion().IsReady());
		EXPECT_EQ(ETaskScopeWaitResult::TimedOut, WaitForTaskGroupForTest(Group, 0.001));
		EXPECT_TRUE(Source.TrySetValue(42));
		EXPECT_FALSE(Source.TrySetValue(100));
		ASSERT_EQ(ETaskState::Canceled, Tasks::Wait(Result.GetCompletion()).TaskState);
		EXPECT_TRUE(std::holds_alternative<Tasks::FTaskCanceled>(std::move(Result).TakeOutcome()));
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
			auto Admission = Tasks::TCompletionSource<int>::TryCreate(Group, {});
			ASSERT_TRUE(Admission.HasValue());
			auto Source = std::move(Admission).TakeValue();
			Abandoned = Source.TakeTask();
		}
		ASSERT_EQ(ETaskState::Failed, Tasks::Wait(Abandoned.GetCompletion()).TaskState);
		auto Failure = std::move(Abandoned).TakeOutcome();
		ASSERT_TRUE(std::holds_alternative<Tasks::FTaskFailure>(Failure));
		EXPECT_EQ(Tasks::ETaskFailureCode::AbandonedSource, std::get<Tasks::FTaskFailure>(Failure).Code);
		auto Admission = Tasks::TCompletionSource<int>::TryCreate(Group, {});
		ASSERT_TRUE(Admission.HasValue());
		auto Source = std::move(Admission).TakeValue();
		auto Result = Source.TakeTask();
		std::atomic<int> Winners = 0;
		std::thread First([&] { if (Source.TrySetValue(7)) ++Winners; });
		std::thread Second([&] { if (Source.TrySetValue(11)) ++Winners; });
		First.join();
		Second.join();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Result.GetCompletion()).TaskState);
		EXPECT_EQ(1, Winners.load());
		const int Value = std::get<int>(std::move(Result).TakeOutcome());
		EXPECT_TRUE(Value == 7 || Value == 11);
		Group.Close();
	}

	TEST(FTaskCompositionTests, ThenAsyncFollowsInnerAndRejectsInnerAdmission)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		Tasks::FTaskExecutionOptions Options;
		auto InnerAdmission = Tasks::TCompletionSource<int>::TryCreate(Group, Options);
		ASSERT_TRUE(InnerAdmission.HasValue());
		auto InnerSource = std::move(InnerAdmission).TakeValue();
		auto RootAdmission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options, [] { return 9; });
		ASSERT_TRUE(RootAdmission.HasValue());
		auto Root = std::move(RootAdmission).TakeValue();
		FThreadEvent Invoked;
		auto OuterAdmission = Tasks::ThenAsync(std::move(Root), Tasks::ETaskExecutor::Worker, Options,
			[Inner = InnerSource.TakeTask(), &Invoked](int Value) mutable {
				EXPECT_EQ(9, Value);
				Invoked.Trigger();
				return std::move(Inner);
			});
		ASSERT_TRUE(OuterAdmission.HasValue());
		auto Outer = std::move(OuterAdmission).TakeValue();
		ASSERT_TRUE(Invoked.WaitFor(1.0));
		EXPECT_FALSE(Outer.GetCompletion().IsReady());
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::Wait(Outer.GetCompletion()).WaitStatus);
		EXPECT_TRUE(InnerSource.TrySetValue(41));
		const auto OuterDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (!Outer.GetCompletion().IsReady() && std::chrono::steady_clock::now() < OuterDeadline) std::this_thread::yield();
		ASSERT_TRUE(Outer.GetCompletion().IsReady());
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Outer.GetCompletion()).TaskState);
		EXPECT_EQ(41, std::get<int>(std::move(Outer).TakeOutcome()));

		auto NextAdmission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options, [] {});
		ASSERT_TRUE(NextAdmission.HasValue());
		auto Next = std::move(NextAdmission).TakeValue();
		auto FailedAdmission = Tasks::ThenAsync(std::move(Next), Tasks::ETaskExecutor::Worker, Options, [] {
			return Tasks::TTaskAdmission<Tasks::TTask<int>>::Failure({Tasks::ETaskAdmissionErrorCode::CapacityExhausted});
		});
		ASSERT_TRUE(FailedAdmission.HasValue());
		auto Failed = std::move(FailedAdmission).TakeValue();
		// Unknown completion can reject GameThread waiting while the callback has not run yet.
		Group.Close();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		auto Outcome = std::move(Failed).TakeOutcome();
		ASSERT_TRUE(std::holds_alternative<Tasks::FTaskFailure>(Outcome));
		const auto& Failure = std::get<Tasks::FTaskFailure>(Outcome);
		ASSERT_TRUE(Failure.AdmissionError.has_value());
		EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::CapacityExhausted, Failure.AdmissionError->Code);
	}

	TEST(FTaskCompositionTests, DynamicDependenciesRejectCycles)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto AAdmission = Tasks::TCompletionSource<void>::TryCreate(Group, {});
		auto BAdmission = Tasks::TCompletionSource<void>::TryCreate(Group, {});
		ASSERT_TRUE(AAdmission.HasValue());
		ASSERT_TRUE(BAdmission.HasValue());
		auto A = std::move(AAdmission).TakeValue();
		auto B = std::move(BAdmission).TakeValue();
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
			auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [Value] { return Value; });
			ASSERT_TRUE(Admission.HasValue());
			Inputs.emplace_back(std::move(Admission).TakeValue());
		}
		auto Rejected = Tasks::WhenAll(std::move(Inputs), static_cast<Tasks::ETaskExecutor>(255));
		ASSERT_FALSE(Rejected.HasValue());
		for (auto& Input : Inputs) EXPECT_TRUE(Input.IsValid());
		auto AllAdmission = Tasks::WhenAll(std::move(Inputs));
		ASSERT_TRUE(AllAdmission.HasValue());
		auto All = std::move(AllAdmission).TakeValue();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(All.GetCompletion()).TaskState);
		EXPECT_EQ((std::vector<int>{4, 2, 8}), std::get<std::vector<int>>(std::move(All).TakeOutcome()));
		auto EmptyAdmission = Tasks::WhenAll(std::vector<Tasks::TTask<int>>{});
		ASSERT_TRUE(EmptyAdmission.HasValue());
		auto Empty = std::move(EmptyAdmission).TakeValue();
		EXPECT_TRUE(Empty.GetCompletion().IsReady());
		EXPECT_TRUE(std::get<std::vector<int>>(std::move(Empty).TakeOutcome()).empty());

		auto A = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {.EstimatedResultBytes = 32}, [] { return std::make_unique<int>(17); });
		auto B = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
		ASSERT_TRUE(A.HasValue());
		ASSERT_TRUE(B.HasValue());
		auto TupleAdmission = Tasks::WhenAll(std::make_tuple(std::move(A).TakeValue(), std::move(B).TakeValue()));
		ASSERT_TRUE(TupleAdmission.HasValue());
		auto Tuple = std::move(TupleAdmission).TakeValue();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Tuple.GetCompletion()).TaskState);
		auto Outcome = std::move(Tuple).TakeOutcome();
		using FValues = std::tuple<std::unique_ptr<int>, std::monostate>;
		ASSERT_TRUE(std::holds_alternative<FValues>(Outcome));
		EXPECT_EQ(17, *std::get<0>(std::get<FValues>(Outcome)));
		Group.Close();
	}

	TEST(FTaskCompositionTests, FanInFailurePrecedesCancellationAndUsesInputOrder)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto FirstAdmission = Tasks::TCompletionSource<int>::TryCreate(Group, {});
		auto SecondAdmission = Tasks::TCompletionSource<int>::TryCreate(Group, {});
		auto CanceledAdmission = Tasks::TCompletionSource<int>::TryCreate(Group, {});
		ASSERT_TRUE(FirstAdmission.HasValue()); ASSERT_TRUE(SecondAdmission.HasValue()); ASSERT_TRUE(CanceledAdmission.HasValue());
		auto First = std::move(FirstAdmission).TakeValue();
		auto Second = std::move(SecondAdmission).TakeValue();
		auto Canceled = std::move(CanceledAdmission).TakeValue();
		std::vector<Tasks::TTask<int>> Inputs;
		Inputs.emplace_back(Canceled.TakeTask());
		Inputs.emplace_back(Second.TakeTask());
		Inputs.emplace_back(First.TakeTask());
		auto Admission = Tasks::WhenAll(std::move(Inputs));
		ASSERT_TRUE(Admission.HasValue());
		auto Result = std::move(Admission).TakeValue();
		EXPECT_TRUE(Canceled.TrySetCanceled());
		EXPECT_TRUE(First.TrySetFailure({}));
		EXPECT_TRUE(Second.TrySetFailure({}));
		ASSERT_EQ(ETaskState::Failed, Tasks::Wait(Result.GetCompletion()).TaskState);
		auto Outcome = std::move(Result).TakeOutcome();
		ASSERT_TRUE(std::holds_alternative<Tasks::FTaskFailure>(Outcome));
		EXPECT_EQ(1u, std::get<Tasks::FTaskFailure>(Outcome).InputIndex);
		EXPECT_EQ(Second.GetCompletion().GetTaskHandle().GetTaskId(), std::get<Tasks::FTaskFailure>(Outcome).RelatedTaskId);
		Group.Close();
	}

	TEST(FTaskCompositionTests, SharedFanInDuplicatesAndAsyncCancellationAreLocal)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		Tasks::FTaskGroup Group;
		auto SourceAdmission = Tasks::TCompletionSource<int>::TryCreate(Group, {});
		ASSERT_TRUE(SourceAdmission.HasValue());
		auto Source = std::move(SourceAdmission).TakeValue();
		auto SourceTask = Source.TakeTask();
		auto SharedAdmission = Tasks::Share(std::move(SourceTask));
		ASSERT_TRUE(SharedAdmission.HasValue());
		auto Shared = std::move(SharedAdmission).TakeValue();
		auto RootAdmission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
		ASSERT_TRUE(RootAdmission.HasValue());
		auto Root = std::move(RootAdmission).TakeValue();
		auto ObserverAdmission = Tasks::ThenAsync(std::move(Root), Tasks::ETaskExecutor::Worker, {}, [Shared] { return Shared; });
		ASSERT_TRUE(ObserverAdmission.HasValue());
		auto Observer = std::move(ObserverAdmission).TakeValue();
		EXPECT_TRUE(Tasks::Cancel(Observer.GetCompletion()));
		std::vector<Tasks::TSharedTask<int>> Inputs{Shared, Shared};
		auto AllAdmission = Tasks::WhenAll(Inputs);
		ASSERT_TRUE(AllAdmission.HasValue());
		auto All = std::move(AllAdmission).TakeValue();
		EXPECT_TRUE(Source.TrySetValue(27));
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(All.GetCompletion()).TaskState);
		EXPECT_EQ(ETaskState::Succeeded, Shared.GetCompletion().GetState());
		using FValues = std::vector<std::shared_ptr<const int>>;
		auto Outcome = std::move(All).TakeOutcome();
		ASSERT_TRUE(std::holds_alternative<FValues>(Outcome));
		const auto& Values = std::get<FValues>(Outcome);
		ASSERT_EQ(2u, Values.size());
		EXPECT_EQ(Values[0].get(), Values[1].get());
		EXPECT_EQ(27, *Values[1]);
		Group.Close();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_TRUE(std::holds_alternative<Tasks::FTaskCanceled>(std::move(Observer).TakeOutcome()));
	}

	TEST(FTaskCompositionTests, ThenAsyncCancelsUniqueDeferredInnerWithoutPumping)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		Tasks::FTaskGroup Group;
		auto RootAdmission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
		ASSERT_TRUE(RootAdmission.HasValue());
		auto Root = std::move(RootAdmission).TakeValue();
		std::atomic<int> Called = 0;
		FThreadEvent Spawned;
		auto Admission = Tasks::ThenAsync(std::move(Root), Tasks::ETaskExecutor::Worker, {}, [&] {
			auto Inner = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::GameThreadDeferred,
				{.EstimatedCaptureBytes = 32}, [&] { ++Called; return 12; });
			Spawned.Trigger();
			return Inner;
		});
		ASSERT_TRUE(Admission.HasValue());
		auto Outer = std::move(Admission).TakeValue();
		ASSERT_TRUE(Spawned.WaitFor(1.0));
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::Wait(Outer.GetCompletion()).WaitStatus);
		EXPECT_TRUE(Tasks::Cancel(Outer.GetCompletion()));
		Group.Close();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_TRUE(std::holds_alternative<Tasks::FTaskCanceled>(std::move(Outer).TakeOutcome()));
		EXPECT_EQ(0, Called.load());
	}

	TEST(FTaskCompositionTests, SchedulerAllocationFailuresRollBackAllReservations)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		FTaskCancellationSource Cancellation;
		Tasks::FTaskExecutionOptions Options;
		Options.Cancellation = Cancellation.GetToken();
		auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options, [] { return 13; });
		ASSERT_TRUE(Admission.HasValue());
		auto Input = std::move(Admission).TakeValue();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Input.GetCompletion()).TaskState);
		// Native readiness precedes final scheduler-accounting release.
		const auto AccountingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while ((GetTaskSchedulerDiagnostics().CurrentTaskReservationCount != 0 || Group.GetDiagnostics().CurrentActiveCount != 0)
			&& std::chrono::steady_clock::now() < AccountingDeadline) std::this_thread::yield();
		ASSERT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
		for (int32 Checkpoint = 1; Checkpoint <= 5; ++Checkpoint)
		{
			Private::SetTaskAdmissionAllocationFailureForTests(Checkpoint);
			auto Failed = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, Options, [](int Value) { return Value; });
			ASSERT_FALSE(Failed.HasValue());
			EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::CapacityExhausted, Failed.GetError().Code);
			ASSERT_TRUE(Input.IsValid());
			EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().CurrentTaskReservationCount);
			EXPECT_EQ(0u, Group.GetDiagnostics().CurrentActiveCount);
		}
		auto Retry = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, Options, [](int Value) { return Value + 1; });
		ASSERT_TRUE(Retry.HasValue());
		auto Result = std::move(Retry).TakeValue();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Result.GetCompletion()).TaskState);
		EXPECT_EQ(14, std::get<int>(std::move(Result).TakeOutcome()));
		Group.Close();
	}

	TEST(FTaskCompositionTests, PostAdmissionAllocationFailureIsTerminalAndConsumesInput)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [] { return 7; });
		ASSERT_TRUE(Admission.HasValue());
		auto Input = std::move(Admission).TakeValue();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Input.GetCompletion()).TaskState);
		Private::SetTaskAdmissionAllocationFailureForTests(6);
		auto Accepted = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, {}, [](int Value) { return Value; });
		ASSERT_TRUE(Accepted.HasValue());
		EXPECT_FALSE(Input.IsValid());
		auto Result = std::move(Accepted).TakeValue();
		EXPECT_TRUE(Result.GetCompletion().IsReady());
		EXPECT_EQ(ETaskTerminalReason::DispatchRejected, Result.GetCompletion().GetTaskHandle().GetDiagnostics().TerminalReason);
		Group.Close();
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
		auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [&](Tasks::FTaskContext& Context) {
			RetainedContext = std::make_unique<Tasks::FTaskContext>(Context.GetCancellationToken(), Group.GetToken());
			ParentStarted.Trigger();
			if (!ContinueParent.WaitFor(2.0)) return;
			auto ChildAdmission = Context.TrySpawnChild(Tasks::ETaskExecutor::Worker, {}, [&] {
				(void)ContinueChild.WaitFor(2.0);
				return 37;
			});
			EXPECT_TRUE(ChildAdmission.HasValue());
			if (ChildAdmission.HasValue()) Child.emplace(std::move(ChildAdmission).TakeValue());
		});
		ASSERT_TRUE(Admission.HasValue());
		auto Parent = std::move(Admission).TakeValue();
		ASSERT_TRUE(ParentStarted.WaitFor(1.0));
		Group.Close();
		auto Join = Group.JoinAsync();
		EXPECT_FALSE(Join.IsReady());
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::Wait(Join).WaitStatus);
		auto Rejected = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [] {});
		ASSERT_FALSE(Rejected.HasValue());
		ContinueParent.Trigger();
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(Parent.GetCompletion()).TaskState);
		ASSERT_TRUE(Child.has_value());
		EXPECT_FALSE(Join.IsReady());
		auto Expired = RetainedContext->TrySpawnChild(Tasks::ETaskExecutor::Worker, {}, [] {});
		ASSERT_FALSE(Expired.HasValue());
		EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::GroupClosed, Expired.GetError().Code);
		ContinueChild.Trigger();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_TRUE(Join.IsReady());
		EXPECT_EQ(37, std::get<int>(std::move(*Child).TakeOutcome()));
	}

	TEST(FTaskCompositionTests, CancelCloseRejectsEvenLiveParentChildren)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		FThreadEvent Started, Continue;
		std::atomic<bool> bRejected = false;
		auto Admission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [&](Tasks::FTaskContext& Context) {
			Started.Trigger();
			if (!Continue.WaitFor(2.0)) return;
			auto Child = Context.TrySpawnChild(Tasks::ETaskExecutor::Worker, {}, [] {});
			bRejected = !Child.HasValue() && Child.GetError().Code == Tasks::ETaskAdmissionErrorCode::GroupClosed;
		});
		ASSERT_TRUE(Admission.HasValue());
		auto Parent = std::move(Admission).TakeValue();
		ASSERT_TRUE(Started.WaitFor(1.0));
		Group.Close();
		EXPECT_EQ(ETaskScopeCloseResult::EscalatedToCancel, Group.Close(ETaskScopeCloseMode::Cancel));
		Continue.Trigger();
		ASSERT_EQ(ETaskState::Canceled, Tasks::Wait(Parent.GetCompletion()).TaskState);
		EXPECT_TRUE(bRejected.load());
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_TRUE(Group.JoinAsync().IsReady());
	}

	TEST(FTaskOperationTests, ReservationsBoundPayloadAndCompleteOnlyAfterOwnerCommit)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		Tasks::TTaskOperationQueue<int> Queue(Group, {.MaxOperations = 1, .MaxPayloadBytes = 4});
		auto Reservation = Queue.TryReserve(11, 2, 4);
		ASSERT_TRUE(Reservation.HasValue());
		auto Ticket = std::move(Reservation).TakeValue();
		auto Completion = Ticket.GetCompletion();
		EXPECT_FALSE(Queue.TryReserve(12, 2, 4).HasValue());
		EXPECT_EQ(4u, Queue.GetReservedBytes());
		auto Build = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {.Cancellation = Ticket.GetCancellationToken()}, [] { return 49; });
		ASSERT_TRUE(Build.HasValue());
		auto Producer = std::move(Build).TakeValue();
		auto ProducerCompletion = Producer.GetCompletion();
		Ticket.Bind(std::move(Producer));
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(ProducerCompletion).TaskState);
		EXPECT_FALSE(Completion.IsReady());
		EXPECT_EQ(ETaskWaitStatus::UnsupportedThread, Tasks::Wait(Completion).WaitStatus);
		int Committed = 0;
		EXPECT_EQ(1u, Queue.Pump(2, [&](uint64 RequestId, int&& Value) { EXPECT_EQ(11u, RequestId); Committed = Value; }));
		EXPECT_EQ(49, Committed);
		EXPECT_EQ(ETaskState::Succeeded, Completion.GetState());
		EXPECT_EQ(0u, Queue.GetReservedBytes());
		Queue.Close(); Group.Close();
	}

	TEST(FTaskOperationTests, ClosePublishesCancellationAndRetainsBudgetUntilProducerAcknowledges)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		Tasks::TTaskOperationQueue<int> Queue(Group, {.MaxOperations = 1, .MaxPayloadBytes = 4});
		auto Reservation = Queue.TryReserve(1, 0, 4);
		ASSERT_TRUE(Reservation.HasValue());
		auto Ticket = std::move(Reservation).TakeValue();
		auto Completion = Ticket.GetCompletion();
		FThreadEvent Started, Continue;
		auto Build = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {.Cancellation = Ticket.GetCancellationToken()}, [&] {
			Started.Trigger(); (void)Continue.WaitFor(2.0); return 7;
		});
		ASSERT_TRUE(Build.HasValue());
		auto Producer = std::move(Build).TakeValue();
		auto ProducerCompletion = Producer.GetCompletion();
		Ticket.Bind(std::move(Producer));
		ASSERT_TRUE(Started.WaitFor(1.0));
		Queue.Close(); Group.Close();
		EXPECT_EQ(ETaskState::Canceled, Completion.GetState());
		EXPECT_FALSE(Group.JoinAsync().IsReady());
		EXPECT_EQ(4u, Queue.GetReservedBytes());
		Continue.Trigger();
		ASSERT_EQ(ETaskState::Canceled, Tasks::Wait(ProducerCompletion).TaskState);
		EXPECT_EQ(0u, Queue.GetReservedBytes());
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_EQ(0u, Queue.Pump(0, [](uint64, int&&) { ADD_FAILURE(); }));
	}

	TEST(FTaskOperationTests, AbandonmentStaleGenerationAndCommitExceptionAreTerminal)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		Tasks::TTaskOperationQueue<int> Queue(Group);
		Tasks::FTaskCompletion Abandoned;
		{
			auto Reservation = Queue.TryReserve(1, 0, 4);
			ASSERT_TRUE(Reservation.HasValue());
			auto Ticket = std::move(Reservation).TakeValue();
			Abandoned = Ticket.GetCompletion();
		}
		EXPECT_EQ(ETaskState::Failed, Abandoned.GetState());
		EXPECT_EQ(0u, Queue.GetActiveCount());
		for (uint64 Generation : {1u, 2u})
		{
			auto Reservation = Queue.TryReserve(Generation, Generation, 4);
			ASSERT_TRUE(Reservation.HasValue());
			auto Ticket = std::move(Reservation).TakeValue();
			auto Completion = Ticket.GetCompletion();
			auto Build = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [] { return 5; });
			ASSERT_TRUE(Build.HasValue());
			auto Producer = std::move(Build).TakeValue();
			auto ProducerCompletion = Producer.GetCompletion();
			Ticket.Bind(std::move(Producer));
			ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(ProducerCompletion).TaskState);
			EXPECT_EQ(1u, Queue.Pump(2, [](uint64, int&&) { throw std::runtime_error("commit failure"); }));
			EXPECT_EQ(Generation == 1 ? ETaskState::Canceled : ETaskState::Failed, Completion.GetState());
		}
		EXPECT_EQ(0u, Queue.GetReservedBytes());
		Queue.Close(); Group.Close();
	}

	TEST(FTaskOperationTests, OwnerDestructionInsideCommitCancelsRemainingRecords)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		auto Queue = std::make_unique<Tasks::TTaskOperationQueue<int>>(Group);
		std::vector<Tasks::FTaskCompletion> Completions;
		for (uint64 Id : {1u, 2u})
		{
			auto Reservation = Queue->TryReserve(Id, 0, 4);
			ASSERT_TRUE(Reservation.HasValue());
			auto Ticket = std::move(Reservation).TakeValue();
			Completions.emplace_back(Ticket.GetCompletion());
			auto Build = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [] { return 3; });
			ASSERT_TRUE(Build.HasValue());
			auto Producer = std::move(Build).TakeValue();
			auto ProducerCompletion = Producer.GetCompletion();
			Ticket.Bind(std::move(Producer));
			ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(ProducerCompletion).TaskState);
		}
		int Calls = 0;
		EXPECT_EQ(1u, Queue->Pump(0, [&](uint64, int&&) { ++Calls; Queue.reset(); }));
		EXPECT_EQ(1, Calls);
		for (const auto& Completion : Completions) EXPECT_EQ(ETaskState::Canceled, Completion.GetState());
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}

	TEST(FTaskCompositionTests, AsyncContextSpawnsChildDuringDrain)
	{
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		FThreadEvent Started, Continue;
		auto RootAdmission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [] { return 6; });
		ASSERT_TRUE(RootAdmission.HasValue());
		auto Root = std::move(RootAdmission).TakeValue();
		auto OuterAdmission = Tasks::ThenAsync(std::move(Root), Tasks::ETaskExecutor::Worker, {},
			[&](Tasks::FTaskContext& Context, int Value) {
				Started.Trigger(); (void)Continue.WaitFor(2.0);
				return Context.TrySpawnChild(Tasks::ETaskExecutor::Worker, {}, [Value] { return Value + 3; });
			});
		ASSERT_TRUE(OuterAdmission.HasValue());
		auto Outer = std::move(OuterAdmission).TakeValue();
		ASSERT_TRUE(Started.WaitFor(1.0));
		Group.Close();
		EXPECT_FALSE(Group.JoinAsync().IsReady());
		Continue.Trigger();
		ASSERT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		EXPECT_EQ(9, std::get<int>(std::move(Outer).TakeOutcome()));
	}

	TEST(FTaskOperationTests, LargePayloadCommitsThroughReservedSlotWithDeferredQueueFull)
	{
		EnsureGameThreadForTaskTest();
		ShutdownTaskScheduler(false);
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor({.MaxQueuedEntries = 1, .MaxQueuedPayloadBytes = 64, .MaxPayloadBytesPerEntry = 64}));
		auto Root = LaunchTask("DeferredSaturationRoot", [] {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);
		auto Deferred = Durin::Then(Root, "SaturatedDeferred", [] {}, {.EstimatedPayloadBytes = 32, .Target = ETaskTarget::GameThreadDeferred});
		ASSERT_TRUE(Deferred.IsValid());
		Tasks::FTaskGroup Group;
		constexpr uint64 PayloadBytes = 2 * 1024 * 1024;
		constexpr uint64 ReservationBytes = PayloadBytes + sizeof(FByteBuffer);
		Tasks::TTaskOperationQueue<FByteBuffer> Queue(Group, {.MaxOperations = 1, .MaxPayloadBytes = ReservationBytes});
		auto Reservation = Queue.TryReserve(1, 0, ReservationBytes);
		ASSERT_TRUE(Reservation.HasValue());
		auto Ticket = std::move(Reservation).TakeValue();
		auto Completion = Ticket.GetCompletion();
		auto Build = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {.EstimatedResultBytes = ReservationBytes}, [] { return FByteBuffer(PayloadBytes, std::byte{7}); });
		ASSERT_TRUE(Build.HasValue());
		auto Producer = std::move(Build).TakeValue();
		auto ProducerCompletion = Producer.GetCompletion();
		Ticket.Bind(std::move(Producer));
		ASSERT_EQ(ETaskState::Succeeded, Tasks::Wait(ProducerCompletion).TaskState);
		EXPECT_EQ(1u, Queue.Pump(0, [PayloadBytes](uint64, FByteBuffer&& Value) { EXPECT_EQ(PayloadBytes, Value.size()); }));
		EXPECT_EQ(ETaskState::Succeeded, Completion.GetState());
		EXPECT_FALSE(Deferred.IsComplete());
		Queue.Close(); Group.Close();
	}

	TEST(FTaskExecutorTests, BlockingIOIsBoundedAndDoesNotOccupyCpuWorkers)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 32, .NumBlockingIOThreads = 1, .MaxBlockingIOTasks = 1}));
		Tasks::FTaskGroup Group;
		FThreadEvent Started;
		FThreadEvent Release;
		auto IO = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::BlockingIO, {}, [&] { Started.Trigger(); Release.Wait(); return 5; });
		ASSERT_TRUE(IO.HasValue());
		ASSERT_TRUE(Started.WaitFor(1.0));
		auto Overflow = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::BlockingIO, {}, [] {});
		EXPECT_FALSE(Overflow.HasValue());
		if (!Overflow.HasValue()) EXPECT_EQ(Tasks::ETaskAdmissionErrorCode::CapacityExhausted, Overflow.GetError().Code);
		FThreadEvent CpuRan;
		auto CPU = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, {}, [&] { CpuRan.Trigger(); });
		EXPECT_TRUE(CPU.HasValue());
		EXPECT_TRUE(CpuRan.WaitFor(1.0));
		Release.Trigger();
		auto Task = std::move(IO).TakeValue();
		EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(Task.GetCompletion()).TaskState);
		Group.Close();
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
	}

	TEST(FTaskExecutorTests, CpuRootsAndContinuationsHonorPriorityWithBoundedOldestService)
	{
		FEngineThreadPoolTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		Tasks::FTaskGroup Group;
		FThreadEvent Started;
		FThreadEvent Release;
		auto Blocker = LaunchTask("PriorityBlocker", [&] { Started.Trigger(); Release.Wait(); });
		ASSERT_TRUE(Started.WaitFor(1.0));
		std::vector<int> Order;
		std::vector<FTaskHandle> Completions;
		Tasks::FTaskExecutionOptions Options;
		Options.Priority = ETaskPriority::Low;
		auto Low = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options, [&] { Order.push_back(-1); });
		ASSERT_TRUE(Low.HasValue());
		Completions.push_back(std::move(Low).TakeValue().GetCompletion().GetTaskHandle());
		auto SourceAdmission = Tasks::TCompletionSource<void>::TryCreate(Group, {});
		ASSERT_TRUE(SourceAdmission.HasValue());
		auto Source = std::move(SourceAdmission).TakeValue();
		auto Input = Source.TakeTask();
		Options.Priority = ETaskPriority::High;
		auto Edge = Tasks::Then(std::move(Input), Tasks::ETaskExecutor::Worker, Options, [&] { Order.push_back(99); });
		ASSERT_TRUE(Edge.HasValue());
		Completions.push_back(std::move(Edge).TakeValue().GetCompletion().GetTaskHandle());
		Source.TrySetValue();
		for (int Index = 0; Index < 24; ++Index)
		{
			auto High = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options, [&, Index] { Order.push_back(Index); });
			ASSERT_TRUE(High.HasValue());
			Completions.push_back(std::move(High).TakeValue().GetCompletion().GetTaskHandle());
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
			auto Parent = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::BlockingIO, {}, [](Tasks::FTaskContext& Context) {
				auto Child = Context.TrySpawnChild(Tasks::ETaskExecutor::BlockingIO, {}, [] { return 11; });
				EXPECT_TRUE(Child.HasValue());
				if (!Child.HasValue()) return;
				auto Task = std::move(Child).TakeValue();
				EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(Task.GetCompletion()).TaskState);
			});
			ASSERT_TRUE(Parent.HasValue());
			auto ParentTask = std::move(Parent).TakeValue();
			EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(ParentTask.GetCompletion()).TaskState);
			FThreadEvent Started, Release;
			auto IO = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::BlockingIO, {}, [&] { Started.Trigger(); Release.Wait(); return 3; });
			ASSERT_TRUE(IO.HasValue());
			auto Task = std::move(IO).TakeValue();
			auto Edge = Tasks::Then(std::move(Task), Tasks::ETaskExecutor::Worker, {}, [](int Value) { return Value + 1; });
			ASSERT_TRUE(Edge.HasValue());
			auto Tail = std::move(Edge).TakeValue();
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
		auto Blocker = LaunchTask("HoldCpu", [&] { CpuBlocked.Trigger(); ReleaseCpu.Wait(); });
		ASSERT_TRUE(CpuBlocked.WaitFor(1.0));
		Tasks::FTaskGroup Group;
		auto Parent = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::BlockingIO, {}, [&](Tasks::FTaskContext& Context) {
			auto Child = Context.TrySpawnChild(Tasks::ETaskExecutor::Worker, {}, [](Tasks::FTaskContext& ChildContext) {
				auto IO = ChildContext.TrySpawnChild(Tasks::ETaskExecutor::BlockingIO, {}, [] {});
				ASSERT_TRUE(IO.HasValue());
				auto Task = std::move(IO).TakeValue();
				EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(Task.GetCompletion()).TaskState);
			});
			ASSERT_TRUE(Child.HasValue());
			auto Task = std::move(Child).TakeValue();
			EXPECT_EQ(ETaskState::Succeeded, Tasks::Wait(Task.GetCompletion()).TaskState);
			Finished.Trigger();
		});
		ASSERT_TRUE(Parent.HasValue());
		const bool CompletedWhileCpuBlocked = Finished.WaitFor(1.0);
		Group.Close(CompletedWhileCpuBlocked ? ETaskScopeCloseMode::Drain : ETaskScopeCloseMode::Cancel);
		ReleaseCpu.Trigger();
		EXPECT_TRUE(CompletedWhileCpuBlocked);
		EXPECT_EQ(ETaskScopeWaitResult::Quiescent, WaitForTaskGroupForTest(Group, 1.0));
		WaitTask(Blocker);
	}
}
