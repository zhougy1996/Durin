#include "TaskKernelTestSupport.h"
#include <gtest/gtest.h>


#include "Threading/Task.h"
#include "Threading/RunnableThread.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{
	namespace
	{
		class FTaskSchedulerQualificationGuard
		{
		public:
			~FTaskSchedulerQualificationGuard()
			{
				ShutdownTaskScheduler(false);
			}
		};
	}

	TEST(FTaskHotPathQualificationTests, SubmissionAndBlockedGraphTraversal)
	{
		FTaskSchedulerQualificationGuard Guard;
		constexpr int Warmups = 2;
		constexpr int Samples = 9;
		auto Report = [](const char* Name, uint32 Width, std::vector<double>& Times) {
			std::cout << "TaskHotPathSamples case=" << Name << " width=" << Width << " us=";
			for (double Time : Times) std::cout << Time << ',';
			std::cout << '\n';
			std::ranges::sort(Times);
			std::cout << "TaskHotPath case=" << Name << " width=" << Width
				<< " warmup=" << Warmups << " samples=" << Samples
				<< " median_us=" << Times[Samples / 2] << " p95_us=" << Times.back() << '\n';
		};
		for (uint32 Producers : {1u, 4u})
		{
			std::vector<double> Times;
			for (int Sample = -Warmups; Sample < Samples; ++Sample)
			{
				ShutdownTaskScheduler(true);
				ASSERT_TRUE(InitializeTaskScheduler(4));
				constexpr uint32 Count = 8192;
				std::vector<Tasks::FTask> Handles(Count);
				std::vector<Tasks::FTaskEvent> Gates;
				for (uint32 Index = 0; Index < Producers; ++Index) Gates.emplace_back("SubmissionGate", false);
				FThreadEvent Start;
				std::atomic<uint32> Ready = 0;
				std::vector<std::thread> Threads;
				for (uint32 Producer = 0; Producer < Producers; ++Producer)
					Threads.emplace_back([&, Producer] {
						++Ready;
						Start.Wait();
						for (uint32 Index = Producer; Index < Count; Index += Producers)
							Handles[Index] = Tasks::LaunchTask("SubmissionMeasure", [] {}, Gates[Producer]);
					});
				while (Ready.load() != Producers) std::this_thread::yield();
				const auto Begin = std::chrono::steady_clock::now();
				Start.Trigger();
				for (auto& Thread : Threads) Thread.join();
				const double Micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Begin).count();
				if (Sample >= 0) Times.push_back(Micros);
				for (auto& Gate : Gates) Gate.Trigger();
				for (const auto& Result : WaitAll(Handles)) EXPECT_EQ(Result.TaskState, ETaskState::Succeeded);
			}
			Report("submit_8192", Producers, Times);
		}
		for (uint32 Depth : {32u, 1024u})
		{
			std::vector<double> Times;
			for (int Sample = -Warmups; Sample < Samples; ++Sample)
			{
				ShutdownTaskScheduler(true);
				ASSERT_TRUE(InitializeTaskScheduler(1));
				Tasks::FTaskEvent Gate("TraversalGate", false);
				Tasks::FTask Tail = Gate;
				for (uint32 Index = 0; Index < Depth; ++Index)
					Tail = Tasks::LaunchTask("BlockedChain", [] {}, Tail);
				double Micros = 0;
				FThreadEvent Finished;
				auto Observer = Tasks::LaunchTask("TraversalObserver", [&] {
					// Exercise worker cycle/authority checks plus directed retraction,
					// without sleep time or task execution obscuring graph-walk cost.
					const auto Begin = std::chrono::steady_clock::now();
					for (uint32 Attempt = 0; Attempt < 64; ++Attempt) EXPECT_FALSE(Tail.TryRetractAndExecute());
					Micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Begin).count();
					Finished.Trigger();
				});
				Finished.Wait();
				Observer.Wait();
				if (Sample >= 0) Times.push_back(Micros);
				Gate.Trigger();
				EXPECT_EQ(Tail.Wait().TaskState, ETaskState::Succeeded);
			}
			Report("retract_64", Depth, Times);
		}
	}

	TEST(FTaskDiagnosticsQualificationTests, FirstSnapshotAfterObservationFreeLifetimeIsNotHistoryLinear)
	{
		FTaskSchedulerQualificationGuard Guard;
		auto RunCohort = [](uint32 TaskCount) -> uint64 {
			ShutdownTaskScheduler(false);
			EXPECT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 4, .TaskWarningThreshold = 100'000}));
			std::vector<Tasks::FTask> Handles;
			Handles.reserve(TaskCount);
			for (uint32 Index = 0; Index < TaskCount; ++Index)
			{
				Handles.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask("LifetimeCounterSoak", []() {})));
			}
			WaitAll(Handles);

			const auto SnapshotStart = std::chrono::steady_clock::now();
			const FTaskSchedulerDiagnostics FirstSnapshot = GetTaskSchedulerDiagnostics();
			const uint64 SnapshotNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - SnapshotStart).count());
			EXPECT_EQ(TaskCount, FirstSnapshot.RetainedTerminalHandleCount);
			EXPECT_EQ(0u, FirstSnapshot.RetainedTerminalResultCount);
			while (GetTaskSchedulerDiagnostics().CompletedTaskCount < TaskCount)
			{
				std::this_thread::yield();
			}
			Handles.clear();
			const auto HandleReleaseDeadline =
				std::chrono::steady_clock::now() + std::chrono::seconds(1);
			while (GetTaskSchedulerDiagnostics().RetainedTerminalHandleCount != 0
				&& std::chrono::steady_clock::now() < HandleReleaseDeadline)
			{
				std::this_thread::yield();
			}
			EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedTerminalHandleCount);
			ShutdownTaskScheduler(true);
			return SnapshotNanoseconds;
		};

		const uint64 SmallLifetimeSnapshotNanoseconds = RunCohort(1'000);
		const uint64 LargeLifetimeSnapshotNanoseconds = RunCohort(50'000);
		EXPECT_LE(LargeLifetimeSnapshotNanoseconds, SmallLifetimeSnapshotNanoseconds * 10 + 1'000'000);
	}

	TEST(FTaskCapacityQualificationTests, ObservationFreeSaturationSoakReconcilesFinalAccounting)
	{
		ShutdownTaskScheduler(false);
		FTaskSchedulerQualificationGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .TaskWarningThreshold = 64}));

		constexpr uint32 RoundCount = 32;
		constexpr uint32 SucceededPerRound = 32;
		constexpr uint32 FailedPerRound = 8;
		constexpr uint32 CanceledPerRound = 24;
		constexpr uint32 OverloadedPerRound = 16;
		const FTaskAttribution Attribution = RegisterTaskAttribution("CapacityTests", "ObservationFreeSoak");
		Tasks::FTaskExecutionOptions RootOptions;
		RootOptions.Attribution = Attribution;

		for (uint32 Round = 0; Round < RoundCount; ++Round)
		{
			FThreadEvent BlockerStarted;
			FThreadEvent ReleaseBlocker;
			Tasks::FTask Blocker = Durin::Tasks::FTask(Tasks::LaunchTask("CapacitySoakBlocker", [&]() {
				BlockerStarted.Trigger();
				ReleaseBlocker.Wait();
			}, RootOptions));
			ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

			Tasks::FTaskExecutionOptions WaitingOptions = RootOptions;
			WaitingOptions.Prerequisites = std::span<const Tasks::FTask>(&Blocker, 1);
			std::vector<Tasks::FTask> Succeeded;
			std::vector<Tasks::FTask> Failed;
			std::vector<Tasks::FTask> Canceled;
			Succeeded.reserve(SucceededPerRound - 1);
			Failed.reserve(FailedPerRound);
			Canceled.reserve(CanceledPerRound);
			for (uint32 Index = 1; Index < SucceededPerRound; ++Index)
			{
				Succeeded.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask("CapacitySoakSucceeded", []() {}, WaitingOptions)));
				ASSERT_TRUE(Succeeded.back().IsValid());
			}
			for (uint32 Index = 0; Index < FailedPerRound; ++Index)
			{
				Failed.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask("CapacitySoakFailed", []() {
					throw std::runtime_error("capacity soak failure");
				}, WaitingOptions)));
				ASSERT_TRUE(Failed.back().IsValid());
			}
			for (uint32 Index = 0; Index < CanceledPerRound; ++Index)
			{
				Canceled.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask("CapacitySoakCanceled", []() {}, WaitingOptions)));
				ASSERT_TRUE(Canceled.back().IsValid());
			}
			for (uint32 Index = 0; Index < OverloadedPerRound; ++Index)
			{
				Succeeded.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask("CapacitySoakOverload", []() {}, RootOptions)));
			}

			for (const Tasks::FTask& Task : Canceled) EXPECT_TRUE(CancelTask(Task));
			ReleaseBlocker.Trigger();
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
			for (const Tasks::FTask& Task : Succeeded) EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
			for (const Tasks::FTask& Task : Failed) EXPECT_EQ(ETaskState::Failed, WaitTask(Task).TaskState);
			for (const Tasks::FTask& Task : Canceled) EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		}

		const FTaskSchedulerDiagnostics Final = GetTaskSchedulerDiagnostics();
		const auto Category = std::ranges::find_if(Final.OwnerCategoryDiagnostics, [](const FTaskOwnerCategoryDiagnostics& Entry) {
			return Entry.Owner == "CapacityTests" && Entry.Category == "ObservationFreeSoak";
		});
		ASSERT_NE(Final.OwnerCategoryDiagnostics.end(), Category);
		const uint64 ExpectedAccepted = static_cast<uint64>(RoundCount)
			* (SucceededPerRound + OverloadedPerRound + FailedPerRound + CanceledPerRound);
		const uint64 ExpectedRejected = 0;
		EXPECT_EQ(ExpectedAccepted, Category->AcceptedCount);
		EXPECT_EQ(static_cast<uint64>(RoundCount) * (SucceededPerRound + OverloadedPerRound + CanceledPerRound), Category->SucceededCount);
		EXPECT_EQ(static_cast<uint64>(RoundCount) * FailedPerRound, Category->FailedCount);
		EXPECT_EQ(0u, Category->CanceledCount);
		EXPECT_EQ(ExpectedRejected, Category->RejectedCount);
		EXPECT_EQ(0u, Category->CurrentNonterminalCount);
		EXPECT_EQ(ExpectedAccepted, Final.CompletedTaskCount);
		EXPECT_EQ(ExpectedRejected, Final.RejectedTaskCount);
		EXPECT_EQ(Category->AcceptedCount,
			Category->SucceededCount + Category->FailedCount + Category->CanceledCount + Category->CurrentNonterminalCount);
		EXPECT_EQ(0u, Final.CurrentTrackedTaskCount);
		EXPECT_EQ(0u, Final.NonterminalTaskCount);
	}

	TEST(FTaskOwnershipQualificationTests, MeasuresCopyableMoveOnlyCallablesAndSharedResultFanout)
	{
		ShutdownTaskScheduler(false);
		FTaskSchedulerQualificationGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(4));
		constexpr uint32 CallableCount = 128;
		constexpr uint32 ResultCount = 32;
		constexpr size_t ResultBytes = 64 * 1'024;

		auto MeasureNanoseconds = [](auto&& Work) -> uint64 {
			const auto Start = std::chrono::steady_clock::now();
			Work();
			return static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - Start).count());
		};

		std::atomic<uint32> CallableRuns = 0;
		const uint64 CopyableCallableNanoseconds = MeasureNanoseconds([&]() {
			std::vector<Tasks::FTask> Handles;
			for (uint32 Index = 0; Index < CallableCount; ++Index)
				Handles.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask("CopyableQualification", [&CallableRuns]() {
					CallableRuns.fetch_add(1, std::memory_order::acq_rel);
				})));
			EXPECT_TRUE(std::ranges::all_of(WaitAll(Handles), [](const FTaskWaitResult& Result) { return Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded; }));
		});
		const uint64 MoveOnlyCallableNanoseconds = MeasureNanoseconds([&]() {
			std::vector<Tasks::FTask> Handles;
			for (uint32 Index = 0; Index < CallableCount; ++Index)
				Handles.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask("MoveOnlyQualification",
					[Value = std::make_unique<uint32>(1), &CallableRuns]() {
						CallableRuns.fetch_add(*Value, std::memory_order::acq_rel);
					})));
			EXPECT_TRUE(std::ranges::all_of(WaitAll(Handles), [](const FTaskWaitResult& Result) { return Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded; }));
		});

		std::atomic<uint64> ResultBytesObserved = 0;
		const uint64 SharedFanoutNanoseconds = MeasureNanoseconds([&]() {
			std::vector<Tasks::FTask> Sinks;
			for (uint32 Index = 0; Index < ResultCount; ++Index)
			{
				auto Producer = Tasks::LaunchTask("SharedTransferQualification", []() {
					return Durin::FByteBuffer(ResultBytes, std::byte{1});
				});
				Sinks.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.DebugName = "SharedTransferSink"}, [&ResultBytesObserved, Producer]() {
					const auto& Value = Producer.GetResult();
					ResultBytesObserved.fetch_add(Value.size(), std::memory_order::acq_rel);
				}, Producer)));
				Sinks.emplace_back(Durin::Tasks::FTask(Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.DebugName = "SharedFanoutSink"}, [&ResultBytesObserved, Producer]() {
						const auto& Value = Producer.GetResult();
						ResultBytesObserved.fetch_add(Value.size(), std::memory_order::acq_rel);
					}, Producer)));
			}
			EXPECT_TRUE(std::ranges::all_of(WaitAll(Sinks), [](const FTaskWaitResult& Result) { return Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded; }));
		});

		EXPECT_EQ(CallableCount * 2, CallableRuns.load(std::memory_order::acquire));
		EXPECT_EQ(static_cast<uint64>(ResultCount) * ResultBytes * 2,
			ResultBytesObserved.load(std::memory_order::acquire));
		std::cout << "TaskOwnershipQualification,callables=" << CallableCount
			<< ",results=" << ResultCount << ",result_bytes=" << ResultBytes
			<< ",copyable_callable_ns=" << CopyableCallableNanoseconds
			<< ",move_only_callable_ns=" << MoveOnlyCallableNanoseconds
			<< ",shared_fanout_ns=" << SharedFanoutNanoseconds << '\n';
	}
	TEST(FTaskPolicyQualificationTests, ReleaseParallelCrossoverSkewAndNestedWork)
	{
		FTaskSchedulerQualificationGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		constexpr int Warmups = 3;
		constexpr int Samples = 30;
		for (uint64 Count : {1024ull, 4096ull, 16384ull, 131072ull})
		{
			for (bool Skewed : {false, true})
			{
				std::vector<uint64> Values(Count);
				for (auto Policy : {Tasks::EParallelForPolicy::Serial, Tasks::EParallelForPolicy::Auto})
				{
					std::vector<double> Times;
					for (int Sample = -Warmups; Sample < Samples; ++Sample)
					{
						const auto Start = std::chrono::steady_clock::now();
						auto Result = Tasks::ParallelFor("PolicyCrossover", Count, [&](uint64 Index) {
							uint64 Value = Index + 1;
							const int Rounds = Skewed && Index < Count / 8 ? 1024 : 64;
							for (int Round = 0; Round < Rounds; ++Round) Value = (Value ^ (Value >> 13)) * 0x9e3779b97f4a7c15ull;
							Values[Index] = Value;
						}, {.Policy = Policy});
						ASSERT_EQ(ETaskState::Succeeded, Result.State);
						const double Micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();
						if (Sample >= 0) Times.push_back(Micros);
					}
					std::ranges::sort(Times);
					std::cout << "TaskPolicy count=" << Count << " skew=" << Skewed << " policy=" << static_cast<int>(Policy)
						<< " warmup=" << Warmups << " samples=" << Samples << " median_us=" << Times[Samples / 2]
						<< " p95_us=" << Times[28] << " checksum=" << Values.back() << '\n';
				}
			}
		}
		std::atomic<uint64> Total = 0;
		auto Outer = Tasks::ParallelFor("NestedPolicy", 16384, [&](uint64) {
			auto Inner = Tasks::ParallelFor("NestedInner", 2, [&](uint64) { ++Total; });
			EXPECT_EQ(1u, Inner.ChunkCount);
		});
		EXPECT_EQ(ETaskState::Succeeded, Outer.State);
		EXPECT_EQ(32768u, Total.load());
	}
	TEST(FTaskPolicyQualificationTests, MixedBlockingAndCpuCompletionLatency)
	{
		FTaskSchedulerQualificationGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		for (auto Target : {ETaskTarget::AnyWorker, ETaskTarget::BlockingIO})
		{

			std::vector<double> Samples;
			for (int Sample = -3; Sample < 30; ++Sample)
			{
				FThreadEvent StartedA, StartedB, Release, CpuDone;
				const auto Executor = Target == ETaskTarget::BlockingIO ? Tasks::ETaskExecutor::BlockingIO : Tasks::ETaskExecutor::Worker;
				auto A = Durin::Tasks::FTask(Tasks::LaunchTask(Executor, {.DebugName = "MixedBlockA"}, [&] { StartedA.Trigger(); Release.Wait(); }));
				auto B = Durin::Tasks::FTask(Tasks::LaunchTask(Executor, {.DebugName = "MixedBlockB"}, [&] { StartedB.Trigger(); Release.Wait(); }));
				ASSERT_TRUE(StartedA.WaitFor(1.0));
				ASSERT_TRUE(StartedB.WaitFor(1.0));
				double Latency = 0;
				const auto Start = std::chrono::steady_clock::now();
				auto CPU = Durin::Tasks::FTask(Tasks::LaunchTask("MixedCpu", [&] {
					Latency = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();
					CpuDone.Trigger();
				}));
				const bool RanWhileBlocked = CpuDone.WaitFor(0.01);
				Release.Trigger();
				EXPECT_EQ(Target == ETaskTarget::BlockingIO, RanWhileBlocked);
				EXPECT_EQ(ETaskState::Succeeded, WaitTask(A).TaskState);
				EXPECT_EQ(ETaskState::Succeeded, WaitTask(B).TaskState);
				EXPECT_EQ(ETaskState::Succeeded, WaitTask(CPU).TaskState);
				if (Sample >= 0) Samples.push_back(Latency);
			}
			std::ranges::sort(Samples);
			std::cout << "TaskMixed target=" << static_cast<int>(Target) << " warmup=3 samples=30 median_us=" << Samples[15]
				<< " p95_us=" << Samples[28] << '\n';
		}
	}
	TEST(FGameThreadDeferredQualificationTests, RepresentativeWorkloadMeasuresAdmissionPumpResidencyAndStaleDrops)
	{
		if (!GIsGameThreadIdInitialized)
		{
			GGameThreadId = FPlatformLTS::GetCurrentThreadId();
			GIsGameThreadIdInitialized = true;
		}
		ShutdownTaskScheduler(false);
		FTaskSchedulerQualificationGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FGameThreadDeferredWorkQueueConfig Config;
		Config.QueuedEntriesWarningThreshold = 256;
		Config.QueuedBytesWarningThreshold = 256 * 64;
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor(Config));
		Tasks::FTask Root = SubmitKernelTask("DeferredMeasurementRoot", []() {});
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Root).TaskState);

		constexpr uint32 CallbackCount = 256;
		constexpr uint64 DeclaredCaptureBytes = 64;
		std::atomic<uint32> ExecutedCount = 0;
		std::vector<Tasks::FTask> Handles;
		Handles.reserve(CallbackCount);
		const auto AdmissionStart = std::chrono::steady_clock::now();
		for (uint32 Index = 0; Index < CallbackCount; ++Index)
		{
			FTaskLaunchOptions Options;
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = DeclaredCaptureBytes;
			Options.Prerequisites = std::span(&Root, 1);
			Options.Attribution = Private::FTaskRuntimeAccess::GetAttribution(Root);
			Handles.emplace_back(SubmitKernelTask("DeferredMeasurementCallback", [&]() {
				ExecutedCount.fetch_add(1, std::memory_order::acq_rel);
			}, Options));
		}
		const uint64 AdmissionNanoseconds = static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - AdmissionStart).count());

		const FGameThreadDeferredWorkQueueDiagnostics BeforePump = GetGameThreadDeferredWorkQueueDiagnostics();
		ASSERT_EQ(CallbackCount, BeforePump.QueueDepth);
		ASSERT_GT(BeforePump.QueuedPayloadBytes, CallbackCount * DeclaredCaptureBytes);
		const FGameThreadDeferredPumpResult Pump = PumpGameThreadDeferredWork({.bUnlimited = true});
		ASSERT_EQ(CallbackCount, Pump.ExecutedCallbacks);
		EXPECT_EQ(CallbackCount, ExecutedCount.load(std::memory_order::acquire));

		uint64 TotalResidencyNanoseconds = 0;
		uint64 MaxResidencyNanoseconds = 0;
		uint32 SucceededCount = 0;
		for (const Tasks::FTask& Handle : Handles)
		{
			const FTaskDiagnostics Diagnostics = Handle.GetDiagnostics();
			TotalResidencyNanoseconds += Diagnostics.QueueResidencyNanoseconds;
			MaxResidencyNanoseconds = std::max(MaxResidencyNanoseconds, Diagnostics.QueueResidencyNanoseconds);
			SucceededCount += Diagnostics.State == ETaskState::Succeeded;
		}
		EXPECT_EQ(CallbackCount, SucceededCount);
		EXPECT_LT(Pump.ElapsedNanoseconds, 1'000'000'000u);
		std::cout << "[ QUALIFICATION ] game_thread_deferred callbacks=" << CallbackCount
			<< " declared_capture_bytes=" << CallbackCount * DeclaredCaptureBytes
			<< " admission_ns=" << AdmissionNanoseconds
			<< " pump_ns=" << Pump.ElapsedNanoseconds
			<< " average_residency_ns=" << TotalResidencyNanoseconds / CallbackCount
			<< " max_residency_ns=" << MaxResidencyNanoseconds
			<< '\n';
	}
}
