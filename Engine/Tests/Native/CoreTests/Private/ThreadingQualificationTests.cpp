#include <gtest/gtest.h>

#include <iostream>

#include "Threading/Task.h"
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

	TEST(FTaskDiagnosticsQualificationTests, FirstSnapshotAfterObservationFreeLifetimeIsNotHistoryLinear)
	{
		FTaskSchedulerQualificationGuard Guard;
		auto RunCohort = [](uint32 TaskCount) -> uint64 {
			ShutdownTaskScheduler(false);
			EXPECT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 4, .MaxNonterminalTasks = 100'000}));
			std::vector<FTaskHandle> Handles;
			Handles.reserve(TaskCount);
			for (uint32 Index = 0; Index < TaskCount; ++Index)
			{
				Handles.emplace_back(LaunchTask("LifetimeCounterSoak", []() {}));
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
		ASSERT_TRUE(InitializeTaskScheduler({.NumWorkerThreads = 1, .MaxNonterminalTasks = 64}));

		constexpr uint32 RoundCount = 32;
		constexpr uint32 SucceededPerRound = 32;
		constexpr uint32 FailedPerRound = 8;
		constexpr uint32 CanceledPerRound = 24;
		constexpr uint32 RejectedPerRound = 16;
		const FTaskAttribution Attribution = RegisterTaskAttribution("CapacityTests", "ObservationFreeSoak");
		FTaskLaunchOptions RootOptions;
		RootOptions.Attribution = Attribution;

		for (uint32 Round = 0; Round < RoundCount; ++Round)
		{
			FThreadEvent BlockerStarted;
			FThreadEvent ReleaseBlocker;
			FTaskHandle Blocker = LaunchTask("CapacitySoakBlocker", [&]() {
				BlockerStarted.Trigger();
				ReleaseBlocker.Wait();
			}, RootOptions);
			ASSERT_TRUE(BlockerStarted.WaitFor(1.0));

			FTaskLaunchOptions WaitingOptions = RootOptions;
			WaitingOptions.Prerequisites = std::span<const FTaskHandle>(&Blocker, 1);
			std::vector<FTaskHandle> Succeeded;
			std::vector<FTaskHandle> Failed;
			std::vector<FTaskHandle> Canceled;
			Succeeded.reserve(SucceededPerRound - 1);
			Failed.reserve(FailedPerRound);
			Canceled.reserve(CanceledPerRound);
			for (uint32 Index = 1; Index < SucceededPerRound; ++Index)
			{
				Succeeded.emplace_back(LaunchTask("CapacitySoakSucceeded", []() {}, WaitingOptions));
				ASSERT_TRUE(Succeeded.back().IsValid());
			}
			for (uint32 Index = 0; Index < FailedPerRound; ++Index)
			{
				Failed.emplace_back(LaunchTask("CapacitySoakFailed", []() {
					throw std::runtime_error("capacity soak failure");
				}, WaitingOptions));
				ASSERT_TRUE(Failed.back().IsValid());
			}
			for (uint32 Index = 0; Index < CanceledPerRound; ++Index)
			{
				Canceled.emplace_back(LaunchTask("CapacitySoakCanceled", []() {}, WaitingOptions));
				ASSERT_TRUE(Canceled.back().IsValid());
			}
			for (uint32 Index = 0; Index < RejectedPerRound; ++Index)
			{
				EXPECT_FALSE(LaunchTask("CapacitySoakRejected", []() {}, RootOptions).IsValid());
			}

			for (const FTaskHandle& Task : Canceled) EXPECT_TRUE(CancelTask(Task));
			ReleaseBlocker.Trigger();
			EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
			for (const FTaskHandle& Task : Succeeded) EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
			for (const FTaskHandle& Task : Failed) EXPECT_EQ(ETaskState::Failed, WaitTask(Task).TaskState);
			for (const FTaskHandle& Task : Canceled) EXPECT_EQ(ETaskState::Canceled, WaitTask(Task).TaskState);
		}

		const FTaskSchedulerDiagnostics Final = GetTaskSchedulerDiagnostics();
		const auto Category = std::ranges::find_if(Final.OwnerCategoryDiagnostics, [](const FTaskOwnerCategoryDiagnostics& Entry) {
			return Entry.Owner == "CapacityTests" && Entry.Category == "ObservationFreeSoak";
		});
		ASSERT_NE(Final.OwnerCategoryDiagnostics.end(), Category);
		const uint64 ExpectedAccepted = static_cast<uint64>(RoundCount)
			* (SucceededPerRound + FailedPerRound + CanceledPerRound);
		const uint64 ExpectedRejected = static_cast<uint64>(RoundCount) * RejectedPerRound;
		EXPECT_EQ(ExpectedAccepted, Category->AcceptedCount);
		EXPECT_EQ(static_cast<uint64>(RoundCount) * SucceededPerRound, Category->SucceededCount);
		EXPECT_EQ(static_cast<uint64>(RoundCount) * FailedPerRound, Category->FailedCount);
		EXPECT_EQ(static_cast<uint64>(RoundCount) * CanceledPerRound, Category->CanceledCount);
		EXPECT_EQ(ExpectedRejected, Category->RejectedCount);
		EXPECT_EQ(0u, Category->CurrentNonterminalCount);
		EXPECT_EQ(ExpectedAccepted, Final.CompletedTaskCount);
		EXPECT_EQ(ExpectedRejected, Final.RejectedTaskCount);
		EXPECT_EQ(ExpectedRejected, Final.CapacityRejectedTaskCount);
		EXPECT_EQ(Category->AcceptedCount,
			Category->SucceededCount + Category->FailedCount + Category->CanceledCount + Category->CurrentNonterminalCount);
		EXPECT_EQ(0u, Final.CurrentTaskReservationCount);
		EXPECT_EQ(0u, Final.NonterminalTaskCount);
	}

	TEST(FTaskOwnershipQualificationTests, MeasuresCopyableMoveOnlyAndSharedUniqueTransfer)
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
			std::vector<FTaskHandle> Handles;
			for (uint32 Index = 0; Index < CallableCount; ++Index)
				Handles.emplace_back(LaunchTask("CopyableQualification", [&CallableRuns]() {
					CallableRuns.fetch_add(1, std::memory_order::acq_rel);
				}));
			EXPECT_TRUE(std::ranges::all_of(WaitAll(Handles), [](const FTaskWaitResult& Result) { return Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded; }));
		});
		const uint64 MoveOnlyCallableNanoseconds = MeasureNanoseconds([&]() {
			std::vector<FTaskHandle> Handles;
			for (uint32 Index = 0; Index < CallableCount; ++Index)
				Handles.emplace_back(LaunchTask("MoveOnlyQualification",
					[Value = std::make_unique<uint32>(1), &CallableRuns]() {
						CallableRuns.fetch_add(*Value, std::memory_order::acq_rel);
					}));
			EXPECT_TRUE(std::ranges::all_of(WaitAll(Handles), [](const FTaskWaitResult& Result) { return Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded; }));
		});

		std::atomic<uint64> ResultBytesObserved = 0;
		const uint64 SharedTransferNanoseconds = MeasureNanoseconds([&]() {
			std::vector<FTaskHandle> Sinks;
			for (uint32 Index = 0; Index < ResultCount; ++Index)
			{
				auto Producer = LaunchTask<Durin::FByteArray>("SharedTransferQualification", []() {
					return Durin::FByteArray(ResultBytes, std::byte{1});
				});
				Sinks.emplace_back(Then(Producer, "SharedTransferSink", [&ResultBytesObserved](const Durin::FByteArray& Value) {
					ResultBytesObserved.fetch_add(Value.size(), std::memory_order::acq_rel);
				}));
			}
			EXPECT_TRUE(std::ranges::all_of(WaitAll(Sinks), [](const FTaskWaitResult& Result) { return Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded; }));
		});
		const uint64 UniqueTransferNanoseconds = MeasureNanoseconds([&]() {
			std::vector<FTaskHandle> Sinks;
			for (uint32 Index = 0; Index < ResultCount; ++Index)
			{
				auto Producer = LaunchUniqueTask<Durin::FByteArray>("UniqueTransferQualification", []() {
					return Durin::FByteArray(ResultBytes, std::byte{1});
				}, {}, ResultBytes);
				Sinks.emplace_back(ConsumeThen(std::move(Producer), "UniqueTransferSink",
					[&ResultBytesObserved](Durin::FByteArray&& Value) {
						ResultBytesObserved.fetch_add(Value.size(), std::memory_order::acq_rel);
					}));
			}
			EXPECT_TRUE(std::ranges::all_of(WaitAll(Sinks), [](const FTaskWaitResult& Result) { return Result.WaitStatus == ETaskWaitStatus::Completed && Result.TaskState == ETaskState::Succeeded; }));
		});

		EXPECT_EQ(CallableCount * 2, CallableRuns.load(std::memory_order::acquire));
		EXPECT_EQ(static_cast<uint64>(ResultCount) * ResultBytes * 2,
			ResultBytesObserved.load(std::memory_order::acquire));
		const auto ResultReleaseDeadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(1);
		while (GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes != 0
			&& std::chrono::steady_clock::now() < ResultReleaseDeadline)
		{
			std::this_thread::yield();
		}
		EXPECT_EQ(0u, GetTaskSchedulerDiagnostics().RetainedUniqueResultBytes);
		std::cout << "TaskOwnershipQualification,callables=" << CallableCount
			<< ",results=" << ResultCount << ",result_bytes=" << ResultBytes
			<< ",copyable_callable_ns=" << CopyableCallableNanoseconds
			<< ",move_only_callable_ns=" << MoveOnlyCallableNanoseconds
			<< ",shared_transfer_ns=" << SharedTransferNanoseconds
			<< ",unique_transfer_ns=" << UniqueTransferNanoseconds << '\n';
	}
}
