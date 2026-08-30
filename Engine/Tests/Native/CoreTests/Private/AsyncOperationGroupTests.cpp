#include "Modules/ModuleTestSupport.h"

#include <gtest/gtest.h>

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"

namespace Durin::Tests
{
	namespace
	{
		class FTaskSystemTestGuard
		{
		public:
			FTaskSystemTestGuard()
			{
				ShutdownTaskScheduler(false);
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
			}

			~FTaskSystemTestGuard()
			{
				ShutdownTaskSystem(ETaskShutdownMode::Cancel);
			}
		};

		auto MakeOptions(FAsyncOperationGroup& Group) -> FTaskLaunchOptions
		{
			FTaskLaunchOptions Options;
			Options.Scope = Group.GetTaskScope();
			Options.CancellationToken = Group.GetCancellationToken();
			return Options;
		}

		auto MakeDeferredOptions(FAsyncOperationGroup& Group) -> FTaskContinuationOptions
		{
			FTaskContinuationOptions Options;
			Options.Scope = Group.GetTaskScope();
			Options.CancellationToken = Group.GetCancellationToken();
			Options.Target = ETaskTarget::GameThreadDeferred;
			Options.EstimatedPayloadBytes = 1;
			return Options;
		}

		class FCancelableAsyncModule final : public IModuleInterface
		{
		public:
			explicit FCancelableAsyncModule(FThreadEvent& InStarted)
				: Started(InStarted)
			{
			}

			auto StartupModule() -> void override
			{
				Group = FModuleStartup::CreateAsyncOperationGroup("BlockingOperation");
				Task = LaunchCancelableTask("CancelableModuleTask", [this](const FTaskCancellationToken& Token) {
					Started.Trigger();
					while (!Token.IsCancellationRequested()) std::this_thread::yield();
				}, MakeOptions(Group));
			}

		private:
			FThreadEvent& Started;
			FAsyncOperationGroup Group;
			FTaskHandle Task;
		};

		class FBlockingAsyncModule final : public IModuleInterface
		{
		public:
			FBlockingAsyncModule(FThreadEvent& InStarted, FThreadEvent& InRelease)
				: Started(InStarted), Release(InRelease)
			{
			}

			auto StartupModule() -> void override
			{
				Group = FModuleStartup::CreateAsyncOperationGroup("BlockingOperation");
				Task = LaunchTask("BlockingModuleTask", [this]() {
					Started.Trigger();
					Release.Wait();
				}, MakeOptions(Group));
			}

			auto WaitForTaskForTest() -> ETaskState { return WaitTask(Task).TaskState; }

		private:
			FThreadEvent& Started;
			FThreadEvent& Release;
			FAsyncOperationGroup Group;
			FTaskHandle Task;
		};
	}

	TEST(FAsyncOperationGroupTests, CloseRejectsDescendantsAndPublishesStableAbortReason)
	{
		FTaskSystemTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FModuleTestOwner Context("AsyncAdmission");
		auto Group = Context.CreateAsyncOperationGroup("Import");
		ASSERT_TRUE(Group.IsValid());

		FThreadEvent Started;
		FThreadEvent Continue;
		FTaskHandle Child;
		FTaskHandle Root = LaunchCancelableTask("AsyncAdmissionRoot", [&](const FTaskCancellationToken& Token) {
			Started.Trigger();
			Continue.Wait();
			EXPECT_TRUE(Token.IsCancellationRequested());
			Child = LaunchTask("RejectedInheritedChild", []() {});
		}, MakeOptions(Group));
		ASSERT_TRUE(Started.WaitFor(1.0));

		EXPECT_EQ(EAsyncOperationCloseStatus::Closed,
			Group.Close(EAsyncOperationCloseMode::Cancel, EAsyncOperationAbortReason::Superseded));
		Continue.Trigger();
		EXPECT_EQ(ETaskState::Canceled, WaitTask(Root).TaskState);
		EXPECT_FALSE(Child.IsValid());
		const auto Drained = Group.Drain(std::chrono::seconds(1));
		ASSERT_TRUE(Drained.Succeeded()) << Drained.Message;
		ASSERT_EQ(1u, Drained.Snapshot.GroupCount);
		EXPECT_EQ(EAsyncOperationAbortReason::Superseded, Drained.Snapshot.Groups[0].AbortReason);
		EXPECT_EQ(0u, Drained.Snapshot.ActiveTaskCount);
		EXPECT_EQ(0u, Drained.Snapshot.RetainedDeferredCallableCount);
	}

	TEST(FAsyncOperationGroupTests, DrainFromOwnedTaskReturnsSelfWait)
	{
		FTaskSystemTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FModuleTestOwner Context("AsyncSelfWait");
		auto Group = Context.CreateAsyncOperationGroup("Build");
		FThreadEvent Started;
		FThreadEvent BeginDrain;
		std::atomic<EAsyncOperationDrainStatus> Observed = EAsyncOperationDrainStatus::Invalid;
		FTaskHandle Task = LaunchTask("AsyncSelfWaitRoot", [&]() {
			Started.Trigger();
			BeginDrain.Wait();
			Observed.store(Group.Drain(std::chrono::seconds(1)).Status, std::memory_order_release);
		}, MakeOptions(Group));
		ASSERT_TRUE(Started.WaitFor(1.0));
		EXPECT_EQ(EAsyncOperationCloseStatus::Closed, Group.Close(EAsyncOperationCloseMode::Drain));
		BeginDrain.Trigger();
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Task).TaskState);
		EXPECT_EQ(EAsyncOperationDrainStatus::SelfWait, Observed.load(std::memory_order_acquire));
		EXPECT_TRUE(Group.Drain(std::chrono::seconds(1)).Succeeded());
	}

	TEST(FAsyncOperationGroupTests, SelectedGameThreadDrainDoesNotRunUnrelatedWork)
	{
		FTaskSystemTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		FModuleTestOwner Context("AsyncDeferred");
		auto First = Context.CreateAsyncOperationGroup("First");
		auto Second = Context.CreateAsyncOperationGroup("Second");
		std::atomic<uint32> FirstRuns = 0;
		std::atomic<uint32> SecondRuns = 0;
		FTaskHandle FirstRoot = LaunchTask("FirstRoot", []() {}, MakeOptions(First));
		FTaskHandle SecondRoot = LaunchTask("SecondRoot", []() {}, MakeOptions(Second));
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(FirstRoot).TaskState);
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(SecondRoot).TaskState);
		FTaskHandle FirstTask = Then(FirstRoot, "FirstDeferred", [&]() { ++FirstRuns; }, MakeDeferredOptions(First));
		FTaskHandle SecondTask = Then(SecondRoot, "SecondDeferred", [&]() { ++SecondRuns; }, MakeDeferredOptions(Second));

		First.Close(EAsyncOperationCloseMode::Drain);
		ASSERT_TRUE(First.Drain(std::chrono::seconds(1)).Succeeded());
		EXPECT_EQ(1u, FirstRuns.load());
		EXPECT_EQ(0u, SecondRuns.load());
		EXPECT_EQ(ETaskState::Queued, SecondTask.GetState());

		Second.Close(EAsyncOperationCloseMode::Drain);
		ASSERT_TRUE(Second.Drain(std::chrono::seconds(1)).Succeeded());
		EXPECT_EQ(1u, SecondRuns.load());
		EXPECT_EQ(ETaskState::Succeeded, FirstTask.GetState());
		EXPECT_EQ(ETaskState::Succeeded, SecondTask.GetState());

		auto Canceled = Context.CreateAsyncOperationGroup("Canceled");
		FTaskHandle CanceledRoot = LaunchTask("CanceledRoot", []() {}, MakeOptions(Canceled));
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(CanceledRoot).TaskState);
		auto Capture = std::make_shared<int>(41);
		std::weak_ptr<int> WeakCapture = Capture;
		std::atomic<bool> bCanceledCallbackRan = false;
		FTaskHandle CanceledTask = Then(CanceledRoot, "CanceledDeferred", [Capture, &bCanceledCallbackRan]() {
			bCanceledCallbackRan.store(true, std::memory_order_release);
		}, MakeDeferredOptions(Canceled));
		Capture.reset();
		ASSERT_FALSE(WeakCapture.expired());
		Canceled.Close(EAsyncOperationCloseMode::Cancel);
		ASSERT_TRUE(Canceled.Drain(std::chrono::seconds(1)).Succeeded());
		EXPECT_EQ(ETaskState::Canceled, CanceledTask.GetState());
		EXPECT_FALSE(bCanceledCallbackRan.load(std::memory_order_acquire));
		EXPECT_TRUE(WeakCapture.expired());
	}

	TEST(FAsyncOperationGroupTests, DrainWaitsForTypedResultsAndQueuedCallableDestruction)
	{
		FTaskSystemTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FModuleTestOwner Context("AsyncStorage");

		auto Results = Context.CreateAsyncOperationGroup("Results");
		auto Typed = LaunchTask<int>("RetainedTypedResult", []() { return 17; }, MakeOptions(Results));
		ASSERT_EQ(ETaskState::Succeeded, WaitTask(Typed.GetTaskHandle()).TaskState);
		Results.Close(EAsyncOperationCloseMode::Drain);
		auto Retained = Results.Drain(std::chrono::milliseconds(1));
		EXPECT_EQ(EAsyncOperationDrainStatus::TimedOut, Retained.Status);
		EXPECT_EQ(1u, Retained.Snapshot.RetainedResultCount);
		Typed = {};
		EXPECT_TRUE(Results.Drain(std::chrono::seconds(1)).Succeeded());

		FThreadEvent BlockerStarted;
		FThreadEvent ReleaseBlocker;
		FTaskHandle Blocker = LaunchTask("CallableQueueBlocker", [&]() {
			BlockerStarted.Trigger();
			ReleaseBlocker.Wait();
		});
		ASSERT_TRUE(BlockerStarted.WaitFor(1.0));
		auto Callables = Context.CreateAsyncOperationGroup("Callables");
		auto Capture = std::make_shared<int>(29);
		std::weak_ptr<int> WeakCapture = Capture;
		FTaskHandle Queued = LaunchTask("RetainedQueuedCallable", [Capture]() {}, MakeOptions(Callables));
		Capture.reset();
		ASSERT_FALSE(WeakCapture.expired());
		Callables.Close(EAsyncOperationCloseMode::Cancel);
		ReleaseBlocker.Trigger();
		ASSERT_TRUE(Callables.Drain(std::chrono::seconds(1)).Succeeded());
		EXPECT_TRUE(WeakCapture.expired());
		EXPECT_EQ(ETaskState::Canceled, Queued.GetState());
		EXPECT_EQ(ETaskState::Succeeded, WaitTask(Blocker).TaskState);
	}

	TEST(FModuleManagerAsyncRetirementTests, UnloadCancelsAndDrainsOwnedOperationsBeforeRelease)
	{
		FTaskSystemTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedAsyncCancel", std::make_unique<FCancelableAsyncModule>(Started)));
		ASSERT_TRUE(Started.WaitFor(1.0));
		const auto Result = FModuleManager::Get().UnloadModule("ManagedAsyncCancel");

		EXPECT_TRUE(Result.Succeeded()) << Result.Message;
		EXPECT_EQ(EModuleState::Unloaded, Result.ObservedState);
		EXPECT_EQ(1u, Result.AsyncOperationSnapshot.GroupCount);
		EXPECT_EQ(0u, Result.AsyncOperationSnapshot.ActiveTaskCount);
		EXPECT_EQ(0u, Result.AsyncOperationSnapshot.RetainedDeferredCallableCount);
	}

	TEST(FModuleManagerAsyncRetirementTests, AsyncTimeoutFailsClosedAndRetainsOperationEvidence)
	{
		FTaskSystemTestGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started;
		FThreadEvent Release;
		auto* Module = static_cast<FBlockingAsyncModule*>(FModuleTestHarness::InstallStartedModule(
			"ManagedAsyncTimeout", std::make_unique<FBlockingAsyncModule>(Started, Release)));
		ASSERT_NE(nullptr, Module);
		ASSERT_TRUE(Started.WaitFor(1.0));
		const auto PreviousTimeout = FModuleTestHarness::SetRetirementTimeout(std::chrono::milliseconds(2));
		const auto Result = FModuleManager::Get().UnloadModule("ManagedAsyncTimeout");
		(void)FModuleTestHarness::SetRetirementTimeout(PreviousTimeout);

		EXPECT_EQ(EModuleOperationStatus::AsyncOperationDrainTimeout, Result.Status);
		EXPECT_EQ(EModuleState::UnloadBlocked, Result.ObservedState);
		EXPECT_EQ(1u, Result.AsyncOperationSnapshot.GroupCount);
		EXPECT_EQ(1u, Result.AsyncOperationSnapshot.ActiveTaskCount);
		EXPECT_NE(nullptr, FModuleManager::Get().FindModule("ManagedAsyncTimeout")->Module.get());
		Release.Trigger();
		EXPECT_EQ(ETaskState::Canceled, Module->WaitForTaskForTest());
	}
}
