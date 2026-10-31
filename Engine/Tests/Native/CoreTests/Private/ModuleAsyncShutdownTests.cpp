#include <gtest/gtest.h>
#include "Modules/ModuleTestSupport.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"

namespace Durin
{
	namespace
	{
		class FTaskSystemGuard
		{
		public:
			~FTaskSystemGuard() { ShutdownTaskScheduler(false); }
		};

		class FModuleWithWorker final : public IModuleInterface
		{
		public:
			explicit FModuleWithWorker(FThreadEvent& InStarted, bool bInRejectPending = false)
				: Started(InStarted), bRejectPending(bInRejectPending) {}
			auto StartupModule() -> void override
			{
				Worker = Tasks::LaunchTask("ModuleOwnedWorker", [this](const FTaskCancellationToken& Token) {
					Started.Trigger();
					while (!Token.IsCancellationRequested()) std::this_thread::yield();
				}, {.Cancellation = Cancellation.GetToken()});
			}
			auto SupportsDynamicReloading() const -> bool override { return true; }
			auto ShutdownModule() -> void override
			{
				if (bRejectPending && !Worker.IsCompleted()) throw std::runtime_error("Module worker is still running");
				CancelAndWait();
			}
			auto CancelAndWait() -> void
			{
				Cancellation.RequestCancellation();
				if (Worker.IsValid()) require(Worker.Wait().WaitStatus == ETaskWaitStatus::Completed);
				Worker = {};
			}
		private:
			FThreadEvent& Started;
			bool bRejectPending;
			FTaskCancellationSource Cancellation;
			Tasks::FTask Worker;
		};
	}

	TEST(FModuleOwnedAsyncShutdownTests, ModuleCancelsAndWaitsForItsOwnWorker)
	{
		FTaskSystemGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedAsyncCancel", std::make_unique<FModuleWithWorker>(Started)));
		ASSERT_TRUE(Started.WaitFor(1.0));
		EXPECT_TRUE(FModuleManager::Get().UnloadModule("ManagedAsyncCancel"));
		EXPECT_EQ(EModuleState::Unloaded, FModuleManager::Get().FindModule("ManagedAsyncCancel")->State.load());
	}

	TEST(FModuleOwnedAsyncShutdownTests, ModuleShutdownFailureKeepsItsInstanceAlive)
	{
		FTaskSystemGuard Guard;
		ASSERT_TRUE(InitializeTaskScheduler(1));
		FThreadEvent Started;
		auto* Module = static_cast<FModuleWithWorker*>(FModuleTestHarness::InstallStartedModule(
			"ManagedAsyncPending", std::make_unique<FModuleWithWorker>(Started, true)));
		ASSERT_NE(nullptr, Module);
		ASSERT_TRUE(Started.WaitFor(1.0));
		EXPECT_THROW(FModuleManager::Get().UnloadModule("ManagedAsyncPending"), std::runtime_error);
		const auto Info = FModuleManager::Get().FindModule("ManagedAsyncPending");
		EXPECT_EQ(EModuleState::ShuttingDown, Info->State.load());
		EXPECT_EQ(Module, Info->Module.get());
		Module->CancelAndWait();
		Info->Module.reset();
		Info->ModuleOwner.reset();
		Info->State = EModuleState::Unloaded;
	}
}
