#include "DynamicUnloadFixtureContract.h"

#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "Modules/ModuleTestSupport.h"
#include "Threading/Task.h"

#include <gtest/gtest.h>

#ifdef _WIN32
	#include <Windows.h>
#elif defined(__APPLE__)
	#include <dlfcn.h>
#endif

namespace Durin::Tests
{
	namespace
	{
#ifdef _WIN32
		constexpr std::string_view FixtureFileName =
			DURIN_RUNTIME_VARIANT "-DynamicUnloadFixture.dll";
#else
		constexpr std::string_view FixtureFileName =
			"lib" DURIN_RUNTIME_VARIANT "-DynamicUnloadFixture.dylib";
#endif

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
			(void)FModularFeatureRegistry::Get().InvokeSingle<IDynamicUnloadFixtureFeature>(
				[](auto& Fixture) { Fixture.SetShutdownTimeout(std::chrono::milliseconds(5)); });
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
#elif defined(__APPLE__)
			if (!Info) return false;
			void* Handle = dlopen(Info->FileName.c_str(), RTLD_LAZY | RTLD_NOLOAD);
			if (!Handle) return false;
			dlclose(Handle);
			return true;
#else
			(void)Info;
			return false;
#endif
		}

		auto ExpectIncompleteAndMapped(
			const FModuleManager::FModuleInfoPtr& Info) -> void
		{
			ASSERT_NE(Info, nullptr);
			EXPECT_EQ(Info->State.load(), EModuleState::ShuttingDown);
			EXPECT_NE(Info->Handle, nullptr);
			EXPECT_NE(Info->Module, nullptr);
			EXPECT_TRUE(IsMapped(Info));
		}
	}

	TEST(FDynamicDllUnloadFailureQualificationTests, ResidentStartupFailureCleansUpWithoutUnmappingCode)
	{
		FFailureHost Host;
		FModuleTestOwner HostContext("ResidentStartupFailureHost");
		auto HostRegistration = HostContext.RegisterFeature<IDynamicUnloadHostFeature>(Host);
		auto& Manager = FModuleManager::Get();
		const FName Name("DynamicUnloadFixtureResidentStartupFailure");
		Manager.AddModule(Name, std::string(FixtureFileName));
		EXPECT_EQ(nullptr, Manager.LoadModule(Name));
		const auto Info = Manager.FindModule(Name);
		ASSERT_NE(nullptr, Info);
		EXPECT_EQ(EModuleState::StoppedMapped, Info->State.load());
		ASSERT_NE(nullptr, Info->Module);
		EXPECT_FALSE(Info->Module->SupportsDynamicReloading());
		EXPECT_NE(nullptr, Info->Handle);
		EXPECT_TRUE(IsMapped(Info));
		EXPECT_TRUE(Host.HasEvent(EDynamicUnloadFixtureEvent::Shutdown, 0));
		EXPECT_FALSE(Host.HasEvent(EDynamicUnloadFixtureEvent::ModuleDestroyed, 0));
		EXPECT_FALSE(Manager.IsModuleLoaded(Name));
		EXPECT_EQ(nullptr, Manager.LoadModule(Name));
		EXPECT_FALSE(Manager.UnloadModule(Name));
		Manager.ShutdownModule(Name);
		EXPECT_EQ(EModuleState::StoppedMapped, Info->State.load());
	}

	TEST(FDynamicDllUnloadFailureQualificationTests,
		ModuleCleanupExceptionsPropagateAndLeaveTheRealImageMapped)
	{
		FTaskSystemGuard TaskGuard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
		FFailureHost Host;
		FModuleTestOwner HostContext("DynamicUnloadQualification.FailureHost");
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
		EXPECT_THROW(Manager.UnloadModule(SyncName), std::runtime_error);
		ExpectIncompleteAndMapped(SyncInfo);
		Host.ReleaseSync(*SyncSerial);
		Caller.join();

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
		EXPECT_THROW(Manager.UnloadModule(WorkerName), std::runtime_error);
		ExpectIncompleteAndMapped(WorkerInfo);
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
		EXPECT_THROW(Manager.UnloadModule(ResultName), std::runtime_error);
		ExpectIncompleteAndMapped(ResultInfo);

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
		EXPECT_THROW(Manager.UnloadModule(DeferredName), std::runtime_error);
		GGameThreadId = FPlatformLTS::GetCurrentThreadId();
		GIsGameThreadIdInitialized = true;
		ExpectIncompleteAndMapped(DeferredInfo);

		const FName ShutdownName("DynamicUnloadFixtureShutdownFailure");
		const auto ShutdownInfo = LoadFixture(ShutdownName);
		ASSERT_NE(ShutdownInfo, nullptr);
		const auto ShutdownSerial = InvokeSerial();
		ASSERT_TRUE(ShutdownSerial.has_value());
		ASSERT_TRUE(InvokeFixture(
			[](IDynamicUnloadFixtureFeature& Fixture) {
				Fixture.SetThrowOnShutdownForFailure();
			}).WasInvoked());
		EXPECT_THROW(Manager.UnloadModule(ShutdownName), std::runtime_error);
		ExpectIncompleteAndMapped(ShutdownInfo);
		EXPECT_FALSE(Host.HasEvent(
			EDynamicUnloadFixtureEvent::ModuleDestroyed, *ShutdownSerial));

		const FName WrongThreadName("DynamicUnloadFixtureWrongThread");
		const auto WrongThreadInfo = LoadFixture(WrongThreadName);
		ASSERT_NE(WrongThreadInfo, nullptr);
		bool bWrongThreadUnload = true;
		std::thread WrongThread([&] {
			bWrongThreadUnload = Manager.UnloadModule(WrongThreadName);
		});
		WrongThread.join();
		EXPECT_FALSE(bWrongThreadUnload);
		EXPECT_EQ(WrongThreadInfo->State.load(), EModuleState::Active);
		EXPECT_TRUE(IsMapped(WrongThreadInfo));
		EXPECT_TRUE(Manager.UnloadModule(WrongThreadName));

		const FName RecursiveName("DynamicUnloadFixtureRecursive");
		const auto RecursiveInfo = LoadFixture(RecursiveName);
		ASSERT_NE(RecursiveInfo, nullptr);
		const auto RecursiveSerial = InvokeSerial();
		ASSERT_TRUE(RecursiveSerial.has_value());
		const auto Recursive = InvokeFixture(
			[](IDynamicUnloadFixtureFeature& Fixture) {
				return Fixture.RequestRecursiveUnloadForFailure();
			});
		EXPECT_EQ(Recursive.Status, EFeatureInvokeStatus::VisitorFailed);
		EXPECT_EQ(RecursiveInfo->State.load(), EModuleState::ShuttingDown);
		ExpectIncompleteAndMapped(RecursiveInfo);
		EXPECT_FALSE(Host.HasEvent(
			EDynamicUnloadFixtureEvent::ModuleDestroyed, *RecursiveSerial));
	}
}
