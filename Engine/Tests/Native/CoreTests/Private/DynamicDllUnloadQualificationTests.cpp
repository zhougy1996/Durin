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
		constexpr std::string_view FixtureModuleName = "DynamicUnloadFixture";

		class FDynamicUnloadHost final : public IDynamicUnloadHostFeature
		{
		public:
			auto AllocateInstanceSerial() -> uint64 override
			{
				return NextInstanceSerial.fetch_add(1, std::memory_order_relaxed);
			}

			auto Record(FDynamicUnloadFixtureEvent Event) -> void override
			{
				std::lock_guard Lock(Mutex);
				Events.push_back(Event);
				Changed.notify_all();
			}

			auto WaitForSynchronousRelease(uint64 InstanceSerial) -> void override
			{
				std::unique_lock Lock(Mutex);
				Changed.wait(Lock, [&] {
					return ReleasedInstances.contains(InstanceSerial);
				});
			}

			auto WaitForAsyncRelease(uint64 InstanceSerial) -> void override
			{
				std::unique_lock Lock(Mutex);
				Changed.wait(Lock, [&] {
					return ReleasedAsyncInstances.contains(InstanceSerial);
				});
			}

			auto ReleaseSynchronous(uint64 InstanceSerial) -> void
			{
				std::lock_guard Lock(Mutex);
				ReleasedInstances.insert(InstanceSerial);
				Changed.notify_all();
			}

			auto WaitFor(
				EDynamicUnloadFixtureEvent Phase,
				uint64 InstanceSerial,
				std::chrono::milliseconds Timeout = std::chrono::seconds(5)) -> bool
			{
				std::unique_lock Lock(Mutex);
				return Changed.wait_for(Lock, Timeout, [&] {
					return std::ranges::any_of(Events, [&](const auto& Event) {
						return Event.Phase == Phase
							&& Event.InstanceSerial == InstanceSerial;
					});
				});
			}

			auto EventIndex(
				EDynamicUnloadFixtureEvent Phase,
				uint64 InstanceSerial) const -> size_t
			{
				std::lock_guard Lock(Mutex);
				const auto It = std::ranges::find_if(Events, [&](const auto& Event) {
					return Event.Phase == Phase
						&& Event.InstanceSerial == InstanceSerial;
				});
				return It == Events.end()
					? std::numeric_limits<size_t>::max()
					: static_cast<size_t>(std::distance(Events.begin(), It));
			}

			auto EventCount(uint64 InstanceSerial) const -> size_t
			{
				std::lock_guard Lock(Mutex);
				return static_cast<size_t>(std::ranges::count_if(
					Events,
					[&](const auto& Event) {
						return Event.InstanceSerial == InstanceSerial;
					}));
			}

		private:
			std::atomic<uint64> NextInstanceSerial = 1;
			mutable std::mutex Mutex;
			std::condition_variable Changed;
			std::vector<FDynamicUnloadFixtureEvent> Events;
			std::unordered_set<uint64> ReleasedInstances;
			std::unordered_set<uint64> ReleasedAsyncInstances;
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

		auto GetFixtureInstanceSerial() -> std::optional<uint64>
		{
			const auto Result = FModularFeatureRegistry::Get()
				.InvokeSingle<IDynamicUnloadFixtureFeature>(
					[](IDynamicUnloadFixtureFeature& Fixture) {
						return Fixture.GetInstanceSerial();
					});
			return Result.WasInvoked() ? Result.Value : std::nullopt;
		}

		auto IsFixtureImageMapped(const FModuleManager::FModuleInfoPtr& ModuleInfo)
			-> bool
		{
#ifdef _WIN32
			if (!ModuleInfo) return false;
			const std::wstring FileName =
				std::filesystem::path(ModuleInfo->FileName).filename().wstring();
			return ::GetModuleHandleW(FileName.c_str()) != nullptr;
#elif defined(__APPLE__)
			if (!ModuleInfo) return false;
			void* Handle = dlopen(ModuleInfo->FileName.c_str(), RTLD_LAZY | RTLD_NOLOAD);
			if (!Handle) return false;
			dlclose(Handle);
			return true;
#else
			(void)ModuleInfo;
			return false;
#endif
		}

		auto ExpectFixtureReleased(
			const FModuleManager::FModuleInfoPtr& ModuleInfo) -> void
		{
			ASSERT_NE(ModuleInfo, nullptr);
			EXPECT_EQ(ModuleInfo->Handle, nullptr);
#ifdef _WIN32
			EXPECT_FALSE(IsFixtureImageMapped(ModuleInfo));
#endif
		}
	}

	TEST(FDynamicDllUnloadQualificationTests,
		SuccessfulUnloadDrainsCallsAsyncStorageAndReloadsNewGenerations)
	{
		FTaskSystemGuard TaskGuard;
		ASSERT_TRUE(InitializeTaskScheduler(2));
		ASSERT_TRUE(InitializeGameThreadDeferredExecutor());

		FDynamicUnloadHost Host;
		FModuleTestOwner HostContext("DynamicUnloadQualification.Host");
		auto HostRegistration =
			HostContext.RegisterFeature<IDynamicUnloadHostFeature>(Host);
		ASSERT_TRUE(HostRegistration.IsValid());

		auto& Manager = FModuleManager::Get();
		ASSERT_NE(Manager.LoadModule(FName(FixtureModuleName)), nullptr);
		const auto FirstInfo = Manager.FindModule(FName(FixtureModuleName));
		ASSERT_NE(FirstInfo, nullptr);
		ASSERT_TRUE(IsFixtureImageMapped(FirstInfo));
		const uint64 FirstGeneration = FirstInfo->OwnerGeneration;
		const auto FirstSerial = GetFixtureInstanceSerial();
		ASSERT_TRUE(FirstSerial.has_value());
		ASSERT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::Startup, *FirstSerial));

		std::atomic<EFeatureInvokeStatus> CallerStatus =
			EFeatureInvokeStatus::Unavailable;
		std::thread Caller([&] {
			CallerStatus = FModularFeatureRegistry::Get()
				.InvokeSingle<IDynamicUnloadFixtureFeature>(
					[](IDynamicUnloadFixtureFeature& Fixture) {
						Fixture.RunSynchronousBarrier();
					}).Status;
		});
		ASSERT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::SynchronousEntered, *FirstSerial));

		std::atomic<EFeatureInvokeStatus> LateStatus =
			EFeatureInvokeStatus::Invoked;
		std::thread Releaser([&] {
			const auto Deadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(5);
			while (FirstInfo->State.load() != EModuleState::Retiring
				&& std::chrono::steady_clock::now() < Deadline)
			{
				std::this_thread::yield();
			}
			LateStatus = FModularFeatureRegistry::Get()
				.InvokeSingle<IDynamicUnloadFixtureFeature>(
					[](IDynamicUnloadFixtureFeature&) {}).Status;
			Host.ReleaseSynchronous(*FirstSerial);
		});
		const FModuleUnloadResult FirstUnload =
			Manager.UnloadModule(FName(FixtureModuleName));
		Releaser.join();
		Caller.join();
		ASSERT_TRUE(FirstUnload.Succeeded()) << FirstUnload.Message;
		EXPECT_EQ(CallerStatus.load(), EFeatureInvokeStatus::Invoked);
		EXPECT_EQ(LateStatus.load(), EFeatureInvokeStatus::Unavailable);
		EXPECT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::SynchronousExited, *FirstSerial));
		EXPECT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::Shutdown, *FirstSerial));
		EXPECT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::ModuleDestroyed, *FirstSerial));
		ExpectFixtureReleased(FirstInfo);

		ASSERT_NE(Manager.LoadModule(FName(FixtureModuleName)), nullptr);
		const auto SecondInfo = Manager.FindModule(FName(FixtureModuleName));
		ASSERT_NE(SecondInfo, nullptr);
		const auto SecondSerial = GetFixtureInstanceSerial();
		ASSERT_TRUE(SecondSerial.has_value());
		EXPECT_GT(SecondInfo->OwnerGeneration, FirstGeneration);
		const uint64 SecondGeneration = SecondInfo->OwnerGeneration;
		EXPECT_GT(*SecondSerial, *FirstSerial);
		const auto Started = FModularFeatureRegistry::Get()
			.InvokeSingle<IDynamicUnloadFixtureFeature>(
				[](IDynamicUnloadFixtureFeature& Fixture) {
					return Fixture.StartDrainedAsyncChain();
				});
		ASSERT_TRUE(Started.WasInvoked());
		ASSERT_TRUE(Started.Value && *Started.Value);

		const FModuleUnloadResult SecondUnload =
			Manager.UnloadModule(FName(FixtureModuleName));
		ASSERT_TRUE(SecondUnload.Succeeded()) << SecondUnload.Message;
		EXPECT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::AsyncPublished, *SecondSerial));
		EXPECT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::AsyncCaptureDestroyed, *SecondSerial));
		EXPECT_TRUE(Host.WaitFor(
			EDynamicUnloadFixtureEvent::ModuleDestroyed, *SecondSerial));
		EXPECT_LT(
			Host.EventIndex(
				EDynamicUnloadFixtureEvent::AsyncCaptureDestroyed,
				*SecondSerial),
			Host.EventIndex(
				EDynamicUnloadFixtureEvent::ModuleDestroyed,
				*SecondSerial));
		ExpectFixtureReleased(SecondInfo);

		ASSERT_NE(Manager.LoadModule(FName(FixtureModuleName)), nullptr);
		const auto ThirdInfo = Manager.FindModule(FName(FixtureModuleName));
		ASSERT_NE(ThirdInfo, nullptr);
		const auto ThirdSerial = GetFixtureInstanceSerial();
		ASSERT_TRUE(ThirdSerial.has_value());
		EXPECT_GT(ThirdInfo->OwnerGeneration, SecondGeneration);
		EXPECT_GT(*ThirdSerial, *SecondSerial);
		const FModuleUnloadResult ThirdUnload =
			Manager.UnloadModule(FName(FixtureModuleName));
		ASSERT_TRUE(ThirdUnload.Succeeded()) << ThirdUnload.Message;
		ExpectFixtureReleased(ThirdInfo);

		const size_t FirstEventCount = Host.EventCount(*FirstSerial);
		const size_t SecondEventCount = Host.EventCount(*SecondSerial);
		const size_t ThirdEventCount = Host.EventCount(*ThirdSerial);
		uint64 PreviousGeneration = ThirdInfo->OwnerGeneration;
		uint64 PreviousSerial = *ThirdSerial;
		for (uint32 Cycle = 0; Cycle < 32; ++Cycle)
		{
			ASSERT_NE(Manager.LoadModule(FName(FixtureModuleName)), nullptr)
				<< "stress cycle " << Cycle;
			const auto CycleInfo = Manager.FindModule(FName(FixtureModuleName));
			ASSERT_NE(CycleInfo, nullptr);
			const auto CycleSerial = GetFixtureInstanceSerial();
			ASSERT_TRUE(CycleSerial.has_value());
			EXPECT_GT(CycleInfo->OwnerGeneration, PreviousGeneration);
			EXPECT_GT(*CycleSerial, PreviousSerial);
			PreviousGeneration = CycleInfo->OwnerGeneration;
			PreviousSerial = *CycleSerial;
			const auto CycleUnload =
				Manager.UnloadModule(FName(FixtureModuleName));
			ASSERT_TRUE(CycleUnload.Succeeded())
				<< "stress cycle " << Cycle << ": " << CycleUnload.Message;
			ExpectFixtureReleased(CycleInfo);
		}
		EXPECT_EQ(Host.EventCount(*FirstSerial), FirstEventCount);
		EXPECT_EQ(Host.EventCount(*SecondSerial), SecondEventCount);
		EXPECT_EQ(Host.EventCount(*ThirdSerial), ThirdEventCount);
	}
}
