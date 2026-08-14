#include "DynamicUnloadFixtureContract.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleTestContext.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>

#ifdef _WIN32
	#include <Windows.h>
#endif

namespace Durin::Tests
{
	namespace
	{
		constexpr std::string_view FixtureFileName =
			DURIN_RUNTIME_VARIANT "-DynamicUnloadFixture.dll";

		class FFailureHost final : public IDynamicUnloadHostFeature
		{
		public:
			auto AllocateInstanceSerial() -> uint64 override
			{
				return NextSerial.fetch_add(1, std::memory_order_relaxed);
			}

			auto Record(FDynamicUnloadFixtureEvent Event) -> void override
			{
				std::lock_guard Lock(Mutex);
				Events.push_back(Event);
				Changed.notify_all();
			}

			auto WaitForSynchronousRelease(uint64 Serial) -> void override
			{
				std::unique_lock Lock(Mutex);
				Changed.wait(Lock, [&] { return SyncReleases.contains(Serial); });
			}

			auto WaitForAsyncRelease(uint64 Serial) -> void override
			{
				std::unique_lock Lock(Mutex);
				Changed.wait(Lock, [&] { return AsyncReleases.contains(Serial); });
			}

			auto ReleaseSync(uint64 Serial) -> void
			{
				std::lock_guard Lock(Mutex);
				SyncReleases.insert(Serial);
				Changed.notify_all();
			}

			auto ReleaseAsync(uint64 Serial) -> void
			{
				std::lock_guard Lock(Mutex);
				AsyncReleases.insert(Serial);
				Changed.notify_all();
			}

			auto WaitFor(EDynamicUnloadFixtureEvent Phase, uint64 Serial) -> bool
			{
				std::unique_lock Lock(Mutex);
				return Changed.wait_for(Lock, std::chrono::seconds(5), [&] {
					return std::ranges::any_of(Events, [&](const auto& Event) {
						return Event.Phase == Phase
							&& Event.InstanceSerial == Serial;
					});
				});
			}

			auto HasEvent(EDynamicUnloadFixtureEvent Phase, uint64 Serial) -> bool
			{
				std::lock_guard Lock(Mutex);
				return std::ranges::any_of(Events, [&](const auto& Event) {
					return Event.Phase == Phase && Event.InstanceSerial == Serial;
				});
			}

		private:
			std::atomic<uint64> NextSerial = 1;
			std::mutex Mutex;
			std::condition_variable Changed;
			std::vector<FDynamicUnloadFixtureEvent> Events;
			std::unordered_set<uint64> SyncReleases;
			std::unordered_set<uint64> AsyncReleases;
		};

		class FTaskSystemGuard
		{
		public:
			FTaskSystemGuard()
			{
				ShutdownTaskScheduler(false);
				if (!GIsGameThreadIdInitialized)
				{
					GGameThreadId = FPlatformLTS::GetCurrentThreadId();
					GIsGameThreadIdInitialized = true;
				}
			}
			~FTaskSystemGuard()
			{
				ShutdownTaskSystem(ETaskShutdownMode::Cancel);
			}
		};

		auto LoadFixture(FName LogicalName) -> FModuleManager::FModuleInfoPtr
		{
			auto& Manager = FModuleManager::Get();
			Manager.AddModule(LogicalName, std::string(FixtureFileName));
			if (!Manager.LoadModule(LogicalName)) return nullptr;
			return Manager.FindModule(LogicalName);
		}

		auto InvokeSerial() -> std::optional<uint64>
		{
			const auto Result = FModularFeatureRegistry::Get()
				.InvokeSingle<IDynamicUnloadFixtureFeature>(
					[](IDynamicUnloadFixtureFeature& Fixture) {
						return Fixture.GetInstanceSerial();
					});
			return Result.WasInvoked() ? Result.Value : std::nullopt;
		}

		template<typename F>
		auto InvokeFixture(F&& Callable)
		{
			return FModularFeatureRegistry::Get()
				.InvokeSingle<IDynamicUnloadFixtureFeature>(
					std::forward<F>(Callable));
		}

		auto IsMapped(const FModuleManager::FModuleInfoPtr& Info) -> bool
		{
#ifdef _WIN32
			if (!Info) return false;
			const std::wstring FileName =
				std::filesystem::path(Info->FileName).filename().wstring();
			return ::GetModuleHandleW(FileName.c_str()) != nullptr;
#else
			(void)Info;
			return false;
#endif
		}

		auto ExpectBlockedAndMapped(
			const FModuleManager::FModuleInfoPtr& Info) -> void
		{
			ASSERT_NE(Info, nullptr);
			EXPECT_EQ(Info->State.load(), EModuleState::UnloadBlocked);
			EXPECT_NE(Info->Handle, nullptr);
			EXPECT_NE(Info->Module, nullptr);
			EXPECT_TRUE(IsMapped(Info));
		}
	}

	TEST(FDynamicDllUnloadFailureQualificationTests,
		EveryInjectedRetirementFailureLeavesTheRealImageMapped)
	{
#ifndef _WIN32
		GTEST_SKIP() << "Physical image qualification currently targets Windows.";
#else
		FTaskSystemGuard TaskGuard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		FFailureHost Host;
		auto HostContext = FModuleTestContextFactory::CreateStartupContext(
			"DynamicUnloadQualification.FailureHost");
		auto HostRegistration =
			HostContext.RegisterFeature<IDynamicUnloadHostFeature>(Host);
		ASSERT_TRUE(HostRegistration.IsValid());
		auto& Manager = FModuleManager::Get();

		const FName SyncName("DynamicUnloadFixtureSyncTimeout");
		const auto SyncInfo = LoadFixture(SyncName);
		ASSERT_NE(SyncInfo, nullptr);
		const auto SyncSerial = InvokeSerial();
		ASSERT_TRUE(SyncSerial.has_value());
		std::thread Caller([] {
			(void)InvokeFixture([](IDynamicUnloadFixtureFeature& Fixture) {
				Fixture.RunSynchronousBarrier();
			});
		});
		ASSERT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::SynchronousEntered, *SyncSerial));
		auto PreviousTimeout = FModuleTestContextFactory::SetRetirementTimeout(
			std::chrono::milliseconds(5));
		const auto SyncUnload = Manager.UnloadModule(SyncName);
		(void)FModuleTestContextFactory::SetRetirementTimeout(PreviousTimeout);
		EXPECT_EQ(SyncUnload.Status,
			EModuleOperationStatus::FeatureInvocationDrainTimeout);
		EXPECT_EQ(SyncUnload.RetirementSnapshot.InFlightInvocationCount, 1u);
		ExpectBlockedAndMapped(SyncInfo);
		Host.ReleaseSync(*SyncSerial);
		Caller.join();

		const FName ResourceName("DynamicUnloadFixtureRetainedResource");
		const auto ResourceInfo = LoadFixture(ResourceName);
		ASSERT_NE(ResourceInfo, nullptr);
		const auto Retained = InvokeFixture(
			[](IDynamicUnloadFixtureFeature& Fixture) {
				return Fixture.RetainOwnerResourceForFailure();
			});
		ASSERT_TRUE(Retained.WasInvoked() && Retained.Value && *Retained.Value);
		const auto ResourceUnload = Manager.UnloadModule(ResourceName);
		EXPECT_EQ(ResourceUnload.Status,
			EModuleOperationStatus::OutstandingFeatureAudit);
		EXPECT_EQ(ResourceUnload.RetirementSnapshot.RetainedResourceCount, 1u);
		ExpectBlockedAndMapped(ResourceInfo);

		const FName WorkerName("DynamicUnloadFixtureWorkerTimeout");
		const auto WorkerInfo = LoadFixture(WorkerName);
		ASSERT_NE(WorkerInfo, nullptr);
		const auto WorkerSerial = InvokeSerial();
		ASSERT_TRUE(WorkerSerial.has_value());
		const auto WorkerStarted = InvokeFixture(
			[](IDynamicUnloadFixtureFeature& Fixture) {
				return Fixture.StartBlockingWorkerForFailure();
			});
		ASSERT_TRUE(WorkerStarted.WasInvoked()
			&& WorkerStarted.Value && *WorkerStarted.Value);
		ASSERT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::BlockingWorkerEntered, *WorkerSerial));
		PreviousTimeout = FModuleTestContextFactory::SetRetirementTimeout(
			std::chrono::milliseconds(5));
		const auto WorkerUnload = Manager.UnloadModule(WorkerName);
		(void)FModuleTestContextFactory::SetRetirementTimeout(PreviousTimeout);
		EXPECT_EQ(WorkerUnload.Status,
			EModuleOperationStatus::AsyncOperationDrainTimeout);
		EXPECT_EQ(WorkerUnload.AsyncOperationSnapshot.ActiveTaskCount, 1u);
		ExpectBlockedAndMapped(WorkerInfo);
		Host.ReleaseAsync(*WorkerSerial);

		const FName ResultName("DynamicUnloadFixtureRetainedResult");
		const auto ResultInfo = LoadFixture(ResultName);
		ASSERT_NE(ResultInfo, nullptr);
		const auto ResultSerial = InvokeSerial();
		ASSERT_TRUE(ResultSerial.has_value());
		const auto ResultStarted = InvokeFixture(
			[](IDynamicUnloadFixtureFeature& Fixture) {
				return Fixture.StartRetainedResultForFailure();
			});
		ASSERT_TRUE(ResultStarted.WasInvoked()
			&& ResultStarted.Value && *ResultStarted.Value);
		ASSERT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::RetainedResultReady, *ResultSerial));
		PreviousTimeout = FModuleTestContextFactory::SetRetirementTimeout(
			std::chrono::milliseconds(5));
		const auto ResultUnload = Manager.UnloadModule(ResultName);
		(void)FModuleTestContextFactory::SetRetirementTimeout(PreviousTimeout);
		EXPECT_EQ(ResultUnload.Status,
			EModuleOperationStatus::AsyncOperationDrainTimeout);
		EXPECT_EQ(ResultUnload.AsyncOperationSnapshot.RetainedResultCount, 1u);
		ExpectBlockedAndMapped(ResultInfo);

		const FName DeferredName("DynamicUnloadFixtureDeferredUnsupported");
		const auto DeferredInfo = LoadFixture(DeferredName);
		ASSERT_NE(DeferredInfo, nullptr);
		const auto DeferredSerial = InvokeSerial();
		ASSERT_TRUE(DeferredSerial.has_value());
		const auto DeferredStarted = InvokeFixture(
			[](IDynamicUnloadFixtureFeature& Fixture) {
				return Fixture.StartDrainedAsyncChain();
			});
		ASSERT_TRUE(DeferredStarted.WasInvoked()
			&& DeferredStarted.Value && *DeferredStarted.Value);
		ASSERT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::AsyncWorkerCompleted, *DeferredSerial));
		GIsGameThreadIdInitialized = false;
		const auto DeferredUnload = Manager.UnloadModule(DeferredName);
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		EXPECT_EQ(DeferredUnload.Status,
			EModuleOperationStatus::AsyncOperationUnsupportedThread);
		EXPECT_GT(
			DeferredUnload.AsyncOperationSnapshot.RetainedDeferredCallableCount,
			0u);
		ExpectBlockedAndMapped(DeferredInfo);

		const FName ReflectedName("DynamicUnloadFixtureReflectedBlock");
		const auto ReflectedInfo = LoadFixture(ReflectedName);
		ASSERT_NE(ReflectedInfo, nullptr);
		const auto ReflectedSerial = InvokeSerial();
		ASSERT_TRUE(ReflectedSerial.has_value());
		Manager.SetPreShutdownModuleCallback(
			[ReflectedName](FName Name) { return Name != ReflectedName; });
		const auto ReflectedUnload = Manager.UnloadModule(ReflectedName);
		Manager.SetPreShutdownModuleCallback({});
		EXPECT_EQ(ReflectedUnload.Status,
			EModuleOperationStatus::ReflectedObjectDrainRejected);
		ExpectBlockedAndMapped(ReflectedInfo);
		EXPECT_FALSE(Host.HasEvent(
			EDynamicUnloadFixtureEvent::ModuleDestroyed, *ReflectedSerial));

		const FName ShutdownName("DynamicUnloadFixtureShutdownFailure");
		const auto ShutdownInfo = LoadFixture(ShutdownName);
		ASSERT_NE(ShutdownInfo, nullptr);
		const auto ShutdownSerial = InvokeSerial();
		ASSERT_TRUE(ShutdownSerial.has_value());
		ASSERT_TRUE(InvokeFixture(
			[](IDynamicUnloadFixtureFeature& Fixture) {
				Fixture.SetThrowOnShutdownForFailure();
			}).WasInvoked());
		const auto ShutdownUnload = Manager.UnloadModule(ShutdownName);
		EXPECT_EQ(ShutdownUnload.Status,
			EModuleOperationStatus::ShutdownCallbackFailure);
		ExpectBlockedAndMapped(ShutdownInfo);
		EXPECT_FALSE(Host.HasEvent(
			EDynamicUnloadFixtureEvent::ModuleDestroyed, *ShutdownSerial));

		const FName WrongThreadName("DynamicUnloadFixtureWrongThread");
		const auto WrongThreadInfo = LoadFixture(WrongThreadName);
		ASSERT_NE(WrongThreadInfo, nullptr);
		FModuleUnloadResult WrongThreadUnload;
		std::thread WrongThread([&] {
			WrongThreadUnload = Manager.UnloadModule(WrongThreadName);
		});
		WrongThread.join();
		EXPECT_EQ(WrongThreadUnload.Status,
			EModuleOperationStatus::WrongControlThread);
		EXPECT_EQ(WrongThreadInfo->State.load(), EModuleState::Active);
		EXPECT_TRUE(IsMapped(WrongThreadInfo));
		EXPECT_TRUE(Manager.UnloadModule(WrongThreadName).Succeeded());

		const FName RecursiveName("DynamicUnloadFixtureRecursive");
		const auto RecursiveInfo = LoadFixture(RecursiveName);
		ASSERT_NE(RecursiveInfo, nullptr);
		const auto RecursiveSerial = InvokeSerial();
		ASSERT_TRUE(RecursiveSerial.has_value());
		const auto Recursive = InvokeFixture(
			[](IDynamicUnloadFixtureFeature& Fixture) {
				return Fixture.RequestRecursiveUnloadForFailure();
			});
		ASSERT_TRUE(Recursive.WasInvoked() && Recursive.Value);
		EXPECT_EQ(*Recursive.Value,
			EModuleOperationStatus::RecursiveOwnedExecution);
		EXPECT_EQ(RecursiveInfo->State.load(), EModuleState::UnloadBlocked);
		ExpectBlockedAndMapped(RecursiveInfo);
		EXPECT_FALSE(Host.HasEvent(
			EDynamicUnloadFixtureEvent::ModuleDestroyed, *RecursiveSerial));
#endif
	}
}
