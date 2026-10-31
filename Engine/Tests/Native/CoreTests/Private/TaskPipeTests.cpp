#include "TaskLifecycleTestSupport.h"
#include "Threading/ThreadEvent.h"
#include "Threading/TaskPipe.h"

namespace Durin
{

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

}
