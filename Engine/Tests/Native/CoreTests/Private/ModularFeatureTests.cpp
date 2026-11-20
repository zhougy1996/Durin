#include "Modules/ModularFeature.h"
#include "Modules/ModuleTestSupport.h"

#include <gtest/gtest.h>

namespace Durin::Tests
{
	class IArithmeticFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.Arithmetic";
		virtual auto AddOne(int Value) -> int = 0;
	};

	class IArithmeticFeatureV2 : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.ArithmeticV2";
		virtual auto AddTwo(int Value) -> int = 0;
	};

	class FArithmeticFeature final : public IArithmeticFeature
	{
	public:
		auto AddOne(int Value) -> int override { return Value + 1; }
	};

	class FArithmeticFeatureV2 final : public IArithmeticFeatureV2
	{
	public:
		auto AddTwo(int Value) -> int override { return Value + 2; }
	};

	class IManagedModuleFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.ManagedModule";
		virtual auto GetValue() -> int = 0;
	};

	struct FManagedModuleObservations
	{
		bool bShutdown = false;
		uint32 ShutdownCount = 0;
		bool bDestroyed = false;
	};

	class FManagedTestModule final : public IModuleInterface, public IManagedModuleFeature
	{
	public:
		explicit FManagedTestModule(FManagedModuleObservations& InObservations, bool bInDynamicReloading = true)
			: Observations(InObservations), bDynamicReloading(bInDynamicReloading) {}
		~FManagedTestModule() override { Observations.bDestroyed = true; }

		auto SupportsDynamicReloading() const -> bool override { return bDynamicReloading; }
		auto SetDynamicReloadingForTest(bool bEnabled) -> void { bDynamicReloading = bEnabled; }
		auto StartupModule() -> void override
		{
			Registration = RegisterModularFeature<IManagedModuleFeature>(*this);
		}
		auto ShutdownModule() -> void override
		{
			// The manager must leave the registration available until our callback.
			EXPECT_TRUE(IModularFeatures::Get().IsModularFeatureAvailable(FName(IManagedModuleFeature::FeatureName)));
			Registration.Reset();
			Observations.bShutdown = true;
			++Observations.ShutdownCount;
		}
		auto GetValue() -> int override { return 42; }

	private:
		FManagedModuleObservations& Observations;
		bool bDynamicReloading;
		FModularFeatureRegistration Registration;
	};

	class FFailingShutdownModule final : public IModuleInterface
	{
	public:
		auto SupportsDynamicReloading() const -> bool override { return true; }
		explicit FFailingShutdownModule(bool& InDestroyed) : bDestroyed(InDestroyed) {}
		~FFailingShutdownModule() override { bDestroyed = true; }
		auto ShutdownModule() -> void override
		{
			throw std::runtime_error("expected shutdown failure");
		}

	private:
		bool& bDestroyed;
	};

	class FStartupNameCaptureModule final : public IModuleInterface
	{
	public:
		explicit FStartupNameCaptureModule(std::vector<FName>& InNames)
			: Names(InNames) {}

		auto StartupModule() -> void override
		{
			Names.push_back(FModuleStartup::GetModuleName());
		}

	private:
		std::vector<FName>& Names;
	};

	class FNestedStartupModule final : public IModuleInterface
	{
	public:
		FNestedStartupModule(
			std::vector<FName>& InNames,
			FModuleTestHarness& InNestedHarness,
			IModuleInterface& InNestedModule)
			: Names(InNames)
			, NestedHarness(InNestedHarness)
			, NestedModule(InNestedModule) {}

		auto StartupModule() -> void override
		{
			Names.push_back(FModuleStartup::GetModuleName());
			NestedHarness.Start(NestedModule);
			Names.push_back(FModuleStartup::GetModuleName());
		}

	private:
		std::vector<FName>& Names;
		FModuleTestHarness& NestedHarness;
		IModuleInterface& NestedModule;
	};

	class FThrowingStartupModule final : public IModuleInterface
	{
	public:
		auto StartupModule() -> void override
		{
			throw std::runtime_error("expected startup failure");
		}
	};

	TEST(FModuleStartupTests, NestedStartupRestoresTheOuterModuleIdentity)
	{
		std::vector<FName> Names;
		FStartupNameCaptureModule NestedModule(Names);
		FModuleTestHarness NestedHarness("NestedStartupB");
		FNestedStartupModule OuterModule(Names, NestedHarness, NestedModule);
		FModuleTestHarness OuterHarness("NestedStartupA");

		OuterHarness.Start(OuterModule);

		ASSERT_EQ(3u, Names.size());
		EXPECT_EQ(FName("NestedStartupA"), Names[0]);
		EXPECT_EQ(FName("NestedStartupB"), Names[1]);
		EXPECT_EQ(FName("NestedStartupA"), Names[2]);

		NestedHarness.Shutdown();
		OuterHarness.Shutdown();
	}

	TEST(FModuleStartupTests, StartupExceptionRestoresThePreviousScope)
	{
		FThrowingStartupModule ThrowingModule;
		FModuleTestHarness ThrowingHarness("ThrowingStartup");
		EXPECT_THROW(ThrowingHarness.Start(ThrowingModule), std::runtime_error);

		std::vector<FName> Names;
		FStartupNameCaptureModule RecoveryModule(Names);
		FModuleTestHarness RecoveryHarness("RecoveryStartup");
		RecoveryHarness.Start(RecoveryModule);

		ASSERT_EQ(1u, Names.size());
		EXPECT_EQ(FName("RecoveryStartup"), Names.front());
		RecoveryHarness.Shutdown();
	}

	TEST(FModularFeatureTests, EnumeratesProvidersByInterfaceName)
	{
		FArithmeticFeature First;
		FArithmeticFeature Second;
		FArithmeticFeatureV2 Other;
		auto FirstRegistration = RegisterModularFeature(First);
		auto SecondRegistration = RegisterModularFeature(Second);
		auto OtherRegistration = RegisterModularFeature(Other);
		auto& Registry = IModularFeatures::Get();

		EXPECT_EQ((std::vector<IArithmeticFeature*>{&First, &Second}),
			Registry.GetModularFeatureImplementations<IArithmeticFeature>(FName(IArithmeticFeature::FeatureName)));
		EXPECT_EQ((std::vector<IArithmeticFeatureV2*>{&Other}),
			Registry.GetModularFeatureImplementations<IArithmeticFeatureV2>(FName(IArithmeticFeatureV2::FeatureName)));

		FirstRegistration.Reset();
		int Visits = 0;
		ForEachModularFeature<IArithmeticFeature>([&](IArithmeticFeature& Feature) {
			++Visits;
			EXPECT_EQ(&Second, &Feature);
			EXPECT_EQ(5, Feature.AddOne(4));
		});
		EXPECT_EQ(1, Visits);
	}

	TEST(FModularFeatureTests, RegistrationTokenHasOneOwner)
	{
		FArithmeticFeature Feature;
		auto Registration = RegisterModularFeature(Feature);
		auto Duplicate = RegisterModularFeature(Feature);
		EXPECT_FALSE(Duplicate.IsValid());
		auto Moved = std::move(Registration);
		EXPECT_FALSE(Registration.IsValid());
		Registration.Reset();
		EXPECT_TRUE(IModularFeatures::Get().IsModularFeatureAvailable(FName(Feature.FeatureName)));
		Moved.Reset();
		Moved.Reset();
		EXPECT_FALSE(IModularFeatures::Get().IsModularFeatureAvailable(FName(Feature.FeatureName)));
	}

	TEST(FModularFeatureTests, NotifiesAfterRegistrationAndImmediateUnregistration)
	{
		auto& Registry = IModularFeatures::Get();
		const FName Type(IArithmeticFeature::FeatureName);
		FArithmeticFeature Feature;
		struct FListener
		{
			const FName& ExpectedType;
			IModularFeature* ExpectedFeature;
			std::vector<int32> Counts;

			auto OnChanged(const FName& Name, IModularFeature* Implementation) -> void
			{
				EXPECT_EQ(ExpectedType, Name);
				EXPECT_EQ(ExpectedFeature, Implementation);
				Counts.push_back(IModularFeatures::Get().GetModularFeatureImplementationCount(Name));
			}
		} Listener{Type, &Feature};
		Registry.OnModularFeatureRegistered().AddRaw(&Listener, &FListener::OnChanged);
		Registry.OnModularFeatureUnregistered().AddRaw(&Listener, &FListener::OnChanged);

		auto Registration = RegisterModularFeature(Feature);
		auto Moved = std::move(Registration);
		EXPECT_TRUE(Registry.IsModularFeatureAvailable(Type));
		Moved.Reset();
		Moved.Reset();
		EXPECT_EQ((std::vector<int32>{1, 0}), Listener.Counts);

		Registry.RegisterModularFeature(Type, &Feature);
		Registry.RegisterModularFeature(Type, &Feature);
		{
			IModularFeatures::FScopedLockModularFeatureList Lock;
			EXPECT_TRUE(Registry.IsModularFeatureAvailable(Type));
			EXPECT_EQ(&Feature, &Registry.GetModularFeature<IArithmeticFeature>(Type));
			EXPECT_EQ((std::vector<IArithmeticFeature*>{&Feature}), Registry.GetModularFeatureImplementations<IArithmeticFeature>(Type));
			EXPECT_EQ(nullptr, Registry.GetModularFeatureImplementation(Type, 1));
		}
		Registry.UnregisterModularFeature(Type, &Feature);
		Registry.UnregisterModularFeature(Type, &Feature);
		EXPECT_FALSE(Registry.IsModularFeatureAvailable(Type));
		EXPECT_EQ((std::vector<int32>{1, 0, 1, 1, 0, 0}), Listener.Counts);

		EXPECT_EQ(1u, Registry.OnModularFeatureRegistered().RemoveAll(&Listener));
		EXPECT_EQ(1u, Registry.OnModularFeatureUnregistered().RemoveAll(&Listener));
		Registry.RegisterModularFeature(Type, &Feature);
		Registry.UnregisterModularFeature(Type, &Feature);
		EXPECT_EQ(6u, Listener.Counts.size());
	}

	TEST(FModularFeatureTests, RemovingRawSubscriberSkipsPendingNotification)
	{
		FArithmeticFeature Feature;
		auto& Registry = IModularFeatures::Get();
		auto& Event = Registry.OnModularFeatureRegistered();
		struct FListener
		{
			int Calls = 0;
			auto OnRegistered(const FName&, IModularFeature*) -> void { ++Calls; }
		} Listener;
		const auto Removal = Event.AddLambda([&](const FName&, IModularFeature*) {
			EXPECT_EQ(1u, Event.RemoveAll(&Listener));
		});
		Event.AddRaw(&Listener, &FListener::OnRegistered);
		auto Registration = RegisterModularFeature(Feature);
		EXPECT_EQ(0, Listener.Calls);
		EXPECT_TRUE(Event.Remove(Removal));
	}

	TEST(FModularFeatureDeathTests, RejectsWrongThreadAccessAndMutationDuringDispatch)
	{
		FArithmeticFeature Feature;
		auto Registration = RegisterModularFeature(Feature);
		EXPECT_DEATH({ std::thread Worker([&] { ForEachModularFeature<IArithmeticFeature>([](auto&) {}); }); Worker.join(); }, "");
		EXPECT_DEATH({ std::thread Worker([&] { auto Token = RegisterModularFeature(Feature); }); Worker.join(); }, "");
		EXPECT_DEATH({ std::thread Worker([&] { Registration.Reset(); }); Worker.join(); }, "");
		EXPECT_DEATH({ ForEachModularFeature<IArithmeticFeature>([&](auto&) { Registration.Reset(); }); }, "");
		EXPECT_DEATH({ ForEachModularFeature<IArithmeticFeature>([&](auto&) { auto Token = RegisterModularFeature(Feature); }); }, "");
		EXPECT_TRUE(Registration.IsValid());
	}

	TEST(FModularFeatureTests, NestedAccessAndExceptionsRestoreTheSafePoint)
	{
		FArithmeticFeature Feature;
		auto Registration = RegisterModularFeature(Feature);
		EXPECT_FALSE(IsModularFeatureDispatching());
		ForEachModularFeature<IArithmeticFeature>([&](auto&) {
			EXPECT_TRUE(IsModularFeatureDispatching());
			EXPECT_THROW(ForEachModularFeature<IArithmeticFeature>([](auto&) {
				throw std::runtime_error("expected nested failure");
			}), std::runtime_error);
			EXPECT_TRUE(IsModularFeatureDispatching());
		});
		EXPECT_FALSE(IsModularFeatureDispatching());
	}

	TEST(FModuleManagerLifecycleTests, FeatureDispatchRejectsReentrantUnloadBeforeShutdown)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"DispatchUnload", std::make_unique<FManagedTestModule>(Observations)));
		std::optional<bool> Unloaded;
		ForEachModularFeature<IManagedModuleFeature>([&](auto&) {
			Unloaded = FModuleManager::Get().UnloadModule("DispatchUnload");
		});
		EXPECT_EQ(false, Unloaded);
		EXPECT_FALSE(Observations.bShutdown);
		EXPECT_TRUE(FModuleManager::Get().UnloadModule("DispatchUnload"));
		EXPECT_TRUE(Observations.bShutdown);
		EXPECT_TRUE(Observations.bDestroyed);
	}

	TEST(FModuleManagerLifecycleTests, UnloadCallsModuleCleanupBeforeDestroyingInstance)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleSuccess", std::make_unique<FManagedTestModule>(Observations)));
		std::optional<int> Value;
		ForEachModularFeature<IManagedModuleFeature>([&](IManagedModuleFeature& Feature) {
			Value = Feature.GetValue();
		});
		EXPECT_EQ(42, Value);

		const auto Result = FModuleManager::Get().UnloadModule("ManagedModuleSuccess");
		EXPECT_TRUE(Result);
		const auto Info = FModuleManager::Get().FindModule("ManagedModuleSuccess");
		EXPECT_EQ(EModuleState::Unloaded, Info->State.load());
		EXPECT_TRUE(Observations.bShutdown);
		EXPECT_TRUE(Observations.bDestroyed);
	}

	TEST(FModuleManagerLifecycleTests, NonReloadableModuleAllowsShutdownButRejectsUnload)
	{
		EXPECT_FALSE(IModuleInterface{}.SupportsDynamicReloading());
		FManagedModuleObservations Observations;
		auto* Module = static_cast<FManagedTestModule*>(FModuleTestHarness::InstallStartedModule(
			"ManagedModuleNonReloadable", std::make_unique<FManagedTestModule>(Observations, false)));
		ASSERT_NE(nullptr, Module);
		auto& Manager = FModuleManager::Get();
		EXPECT_FALSE(Manager.UnloadModule("ManagedModuleNonReloadable"));
		EXPECT_TRUE(Manager.IsModuleLoaded("ManagedModuleNonReloadable"));
		EXPECT_FALSE(Observations.bShutdown);
		Manager.ShutdownModule("ManagedModuleNonReloadable");
		Manager.ShutdownModule("ManagedModuleNonReloadable");
		EXPECT_EQ(1u, Observations.ShutdownCount);
		EXPECT_FALSE(Manager.IsModuleLoaded("ManagedModuleNonReloadable"));
		EXPECT_EQ(nullptr, Manager.LoadModule("ManagedModuleNonReloadable"));
		EXPECT_EQ(nullptr, Manager.GetModule("ManagedModuleNonReloadable"));
		EXPECT_EQ(EModuleState::StoppedMapped, Manager.FindModule("ManagedModuleNonReloadable")->State.load());
		EXPECT_FALSE(Manager.UnloadModule("ManagedModuleNonReloadable"));
		EXPECT_FALSE(Observations.bDestroyed);
		Module->SetDynamicReloadingForTest(true);
		EXPECT_TRUE(Manager.UnloadModule("ManagedModuleNonReloadable"));
		EXPECT_EQ(1u, Observations.ShutdownCount);
		EXPECT_TRUE(Observations.bDestroyed);
	}

	TEST(FModuleManagerLifecycleTests, UnsafeModulePostponesShutdownWithoutMutation)
	{
		class FSafePointModule final : public IModuleInterface
		{
		public:
			auto SupportsDynamicReloading() const -> bool override { return true; }
			auto CanShutdownModule() const -> bool override { return Safe; }
			auto ShutdownModule() -> void override { ++ShutdownCount; }
			bool Safe = false;
			int ShutdownCount = 0;
		};
		auto& Manager = FModuleManager::Get();
		auto* Module = static_cast<FSafePointModule*>(FModuleTestHarness::InstallStartedModule("ManagedModuleSafePoint", std::make_unique<FSafePointModule>()));
		ASSERT_NE(nullptr, Module);
		EXPECT_FALSE(Manager.UnloadModule("ManagedModuleSafePoint"));
		EXPECT_EQ(0, Module->ShutdownCount); EXPECT_TRUE(Manager.IsModuleLoaded("ManagedModuleSafePoint"));
		Module->Safe = true;
		Manager.ShutdownModule("ManagedModuleSafePoint"); EXPECT_EQ(1, Module->ShutdownCount);
		Module->Safe = false; // A stopped module never re-runs its safe-point check or shutdown hook.
		EXPECT_TRUE(Manager.UnloadModule("ManagedModuleSafePoint"));
	}

	TEST(FModuleManagerLifecycleTests, WrongThreadUnloadDoesNotChangeState)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleWrongThread", std::make_unique<FManagedTestModule>(Observations)));
		bool bWrongThreadResult = true;
		std::thread Worker([&]() {
			bWrongThreadResult = FModuleManager::Get().UnloadModule("ManagedModuleWrongThread");
		});
		Worker.join();
		EXPECT_FALSE(bWrongThreadResult);
		EXPECT_FALSE(Observations.bShutdown);
		EXPECT_TRUE(FModuleManager::Get().UnloadModule("ManagedModuleWrongThread"));
	}

	TEST(FModuleManagerLifecycleTests, ShutdownFailurePropagatesAndDoesNotReleaseOrRetry)
	{
		bool bDestroyed = false;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleShutdownFailure", std::make_unique<FFailingShutdownModule>(bDestroyed)));
		auto& Manager = FModuleManager::Get();
		EXPECT_THROW(Manager.UnloadModule("ManagedModuleShutdownFailure"), std::runtime_error);
		const auto Info = Manager.FindModule("ManagedModuleShutdownFailure");
		EXPECT_EQ(EModuleState::ShuttingDown, Info->State.load());
		EXPECT_FALSE(bDestroyed);
		EXPECT_FALSE(Manager.UnloadModule("ManagedModuleShutdownFailure"));
		EXPECT_EQ(nullptr, Manager.LoadModule("ManagedModuleShutdownFailure"));
		// In-memory fixture only: avoid retaining its reference to a stack observation.
		Info->Module.reset();
		Info->ModuleOwner.reset();
		Info->State = EModuleState::Unloaded;
	}

	TEST(FModuleManagerLifecycleTests, ExitUsesReverseStartupCompletionAndSkipsAlreadyStoppedModules)
	{
		class FOrderedModule final : public IModuleInterface
		{
		public:
			FOrderedModule(int InId, std::vector<int>& InOrder) : Id(InId), Order(InOrder) {}
			auto StartupModule() -> void override
			{
				if (Id == 1)
					FModuleTestHarness::InstallStartedModule("ExitOrderNested", std::make_unique<FOrderedModule>(0, Order));
			}
			auto ShutdownModule() -> void override
			{
				if (Id == 1) EXPECT_TRUE(FModuleManager::Get().IsModuleLoaded("ExitOrderNested"));
				Order.push_back(Id);
			}
			int Id;
			std::vector<int>& Order;
		};
		std::vector<int> Order;
		auto& Manager = FModuleManager::Get();
		FModuleTestHarness::InstallStartedModule("ExitOrderFirst", std::make_unique<FOrderedModule>(1, Order));
		FModuleTestHarness::InstallStartedModule("ExitOrderSecond", std::make_unique<FOrderedModule>(2, Order));
		FModuleTestHarness::InstallStartedModule("ExitOrderThird", std::make_unique<FOrderedModule>(3, Order));
		FModuleTestHarness::InstallStartedModule("ExitOrderDeferred", std::make_unique<FOrderedModule>(4, Order));
		Manager.ShutdownModule("ExitOrderSecond");
		const std::array Deferred{FName("ExitOrderDeferred")};
		Manager.ShutdownModulesAtExit(Deferred);
		EXPECT_EQ((std::vector<int>{2, 3, 1, 0}), Order);
		EXPECT_TRUE(Manager.IsModuleLoaded("ExitOrderDeferred"));
		Manager.ShutdownModulesAtExit();
		EXPECT_EQ((std::vector<int>{2, 3, 1, 0, 4}), Order);
		for (const auto Name : {"ExitOrderFirst", "ExitOrderSecond", "ExitOrderThird", "ExitOrderDeferred", "ExitOrderNested"})
		{
			const auto Info = Manager.FindModule(Name);
			EXPECT_NE(nullptr, Info->Module);
			EXPECT_EQ(EModuleState::StoppedMapped, Info->State.load());
			Info->Module.reset();
			Info->ModuleOwner.reset();
			Info->State = EModuleState::Unloaded;
		}
	}
}
