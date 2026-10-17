#include "Modules/ModularFeature.h"
#include "Modules/ModuleTestSupport.h"

#include <gtest/gtest.h>

namespace Durin::Tests
{
	class IArithmeticFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.Arithmetic";
		static constexpr uint32 FeatureVersion = 1;
		virtual auto AddOne(int Value) -> int = 0;
	};

	class IArithmeticFeatureV2 : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.Arithmetic";
		static constexpr uint32 FeatureVersion = 2;
		virtual auto AddTwo(int Value) -> int = 0;
	};

	class IInvalidFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.Invalid";
		static constexpr uint32 FeatureVersion = 0;
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

	class FInvalidFeature final : public IInvalidFeature
	{
	};

	class IManagedModuleFeature : public IModularFeature
	{
	public:
		static constexpr std::string_view FeatureName = "Tests.ManagedModule";
		static constexpr uint32 FeatureVersion = 1;
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
			Registration = FModularFeatureRegistry::Get().RegisterFeature<IManagedModuleFeature>(*this);
		}
		auto ShutdownModule() -> void override
		{
			// The manager must leave the registration available until our callback.
			EXPECT_EQ(EFeatureInvokeStatus::Invoked,
				FModularFeatureRegistry::Get().InvokeSingle<IManagedModuleFeature>([](auto&) {}).Status);
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

	TEST(FModularFeatureTests, UsesDeclaredNameAndVersionAndRejectsInvalidIdentity)
	{

		FArithmeticFeature V1;
		FArithmeticFeatureV2 V2;
		FInvalidFeature Invalid;
		auto V1Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(V1);
		auto V2Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(V2);
		auto InvalidRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Invalid);

		EXPECT_TRUE(V1Registration.IsValid());
		EXPECT_TRUE(V2Registration.IsValid());
		EXPECT_FALSE(InvalidRegistration.IsValid());
		const auto V1Result = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>(
			[](IArithmeticFeature& Feature) { return Feature.AddOne(4); });
		const auto V2Result = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeatureV2>(
			[](IArithmeticFeatureV2& Feature) { return Feature.AddTwo(4); });
		ASSERT_TRUE(V1Result.Value.has_value());
		ASSERT_TRUE(V2Result.Value.has_value());
		EXPECT_EQ(5, *V1Result.Value);
		EXPECT_EQ(6, *V2Result.Value);
	}

	TEST(FModularFeatureTests, ExpectedRegistrationRejectsReplacementBeforeEnteringVisitor)
	{

		FArithmeticFeature Feature;
		auto Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Feature);
		const auto First = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>(
			[](IArithmeticFeature& Value) { return Value.AddOne(1); });
		ASSERT_TRUE(First.WasInvoked());
		ASSERT_NE(0u, First.RegistrationIdentity);
		Registration.Reset();
		EXPECT_FALSE(Registration.IsValid());
		Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Feature);
		bool bEntered = false;
		const auto Stale = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>(
			[&](IArithmeticFeature&) { bEntered = true; }, First.RegistrationIdentity);
		EXPECT_FALSE(Stale.WasInvoked());
		EXPECT_FALSE(bEntered);
		EXPECT_NE(First.RegistrationIdentity, Stale.RegistrationIdentity);
		const auto Current = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>(
			[&](IArithmeticFeature&) { bEntered = true; }, Stale.RegistrationIdentity);
		EXPECT_TRUE(Current.WasInvoked());
		EXPECT_TRUE(bEntered);
		const auto All = FModularFeatureRegistry::Get().InvokeAll<IArithmeticFeature>(
			[](IArithmeticFeature& Value) { return Value.AddOne(2); });
		ASSERT_EQ(1u, All.Invocations.size());
		EXPECT_EQ(Current.RegistrationIdentity, All.Invocations.front().RegistrationIdentity);
		Registration.Reset();
		EXPECT_FALSE(Registration.IsValid());
	}

	TEST(FModularFeatureTests, ExactInvocationSelectsOnlyTheRequestedIdentityAndType)
	{

		FArithmeticFeature First;
		FArithmeticFeature Second;
		auto FirstRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(First);
		const auto Discovered = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](auto&) {});
		ASSERT_TRUE(Discovered.WasInvoked());
		auto SecondRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Second);
		auto& Registry = FModularFeatureRegistry::Get();
		const auto Result = Registry.InvokeRegistered<IArithmeticFeature>(Discovered.RegistrationIdentity, [&](auto& Feature) {
			EXPECT_EQ(&First, &Feature);
			EXPECT_TRUE(FModularFeatureRegistry::Get().IsDispatching());
			return Feature.AddOne(5);
		});
		ASSERT_TRUE(Result.WasInvoked());
		EXPECT_EQ(6, *Result.Value);
		EXPECT_EQ(1u, Result.MatchingRegistrationCount);
		EXPECT_EQ(Discovered.RegistrationIdentity, Result.RegistrationIdentity);
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable, Registry.InvokeRegistered<IArithmeticFeatureV2>(Discovered.RegistrationIdentity, [](auto&) {}).Status);
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable, Registry.InvokeRegistered<IArithmeticFeature>(0, [](auto&) {}).Status);
		FirstRegistration.Reset();
		EXPECT_FALSE(FirstRegistration.IsValid());
		FirstRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(First);
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable, Registry.InvokeRegistered<IArithmeticFeature>(Discovered.RegistrationIdentity, [](auto&) {}).Status);
	}

	TEST(FModularFeatureTests, PublicationRevisionTracksRegistrationAndImmediateUnregistration)
	{
		auto& Registry = FModularFeatureRegistry::Get();
		const auto Before = Registry.GetPublicationRevision();

		FArithmeticFeature Feature;
		FInvalidFeature Invalid;
		auto InvalidRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Invalid);
		EXPECT_EQ(Before, Registry.GetPublicationRevision());
		auto Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Feature);
		EXPECT_EQ(Before + 1, Registry.GetPublicationRevision());
		auto Moved = std::move(Registration);
		EXPECT_TRUE(Registry.InvokeSingle<IArithmeticFeature>([](auto&) {}).WasInvoked());
		EXPECT_EQ(Before + 1, Registry.GetPublicationRevision());
		Moved.Reset();
		EXPECT_EQ(Before + 2, Registry.GetPublicationRevision());
		Moved.Reset();
		EXPECT_EQ(Before + 2, Registry.GetPublicationRevision());
		EXPECT_EQ(0u, Durin::FModularFeatureRegistry::Get().InvokeAll<IArithmeticFeature>([](auto&) {}).MatchingRegistrationCount);
	}

	TEST(FModularFeatureTests, ExactInvocationContainsExceptionsAndRestoresTheSafePoint)
	{

		FArithmeticFeature Feature;
		auto Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Feature);
		auto& Registry = FModularFeatureRegistry::Get();
		const auto Discovery = Registry.InvokeSingle<IArithmeticFeature>([](auto&) {});
		const auto Failure = Registry.InvokeRegistered<IArithmeticFeature>(Discovery.RegistrationIdentity, [](auto&) -> int {
			throw std::runtime_error("expected exact invocation failure");
		});
		EXPECT_EQ(EFeatureInvokeStatus::VisitorFailed, Failure.Status);
		EXPECT_FALSE(Failure.Value.has_value());
		EXPECT_FALSE(Registry.IsDispatching());
		Registration.Reset();
		EXPECT_FALSE(Registration.IsValid());
	}

	TEST(FModularFeatureDeathTests, RejectsWrongThreadAccessAndMutationDuringDispatch)
	{

		FArithmeticFeature Feature;
		auto Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Feature);
		auto& Registry = FModularFeatureRegistry::Get();
		EXPECT_DEATH({ std::thread Worker([&] { (void)Registry.InvokeSingle<IArithmeticFeature>([](auto&) {}); }); Worker.join(); }, "");
		EXPECT_DEATH({ std::thread Worker([&] { (void)Registry.GetPublicationRevision(); }); Worker.join(); }, "");
		EXPECT_DEATH({ std::thread Worker([&] { auto Token = Durin::FModularFeatureRegistry::Get().RegisterFeature(Feature); }); Worker.join(); }, "");
		EXPECT_DEATH({ std::thread Worker([&] { Registration.Reset(); }); Worker.join(); }, "");
		EXPECT_DEATH({ (void)Registry.InvokeSingle<IArithmeticFeature>([&](auto&) { Registration.Reset(); }); }, "");
		EXPECT_DEATH({ (void)Registry.InvokeSingle<IArithmeticFeature>([&](auto&) { auto Token = Durin::FModularFeatureRegistry::Get().RegisterFeature(Feature); }); }, "");
		EXPECT_TRUE(Registration.IsValid());
	}

	TEST(FModularFeatureTests, ReportsUnavailableAmbiguousAndInvokesSelectedSet)
	{

		FArithmeticFeature First;
		FArithmeticFeature Second;

		const auto Unavailable = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>(
			[](IArithmeticFeature& Feature) { return Feature.AddOne(0); });
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable, Unavailable.Status);

		auto FirstRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(First);
		auto SecondRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Second);
		const auto Ambiguous = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>(
			[](IArithmeticFeature& Feature) { return Feature.AddOne(0); });
		EXPECT_EQ(EFeatureInvokeStatus::Ambiguous, Ambiguous.Status);
		EXPECT_EQ(2u, Ambiguous.MatchingRegistrationCount);

		const auto All = FModularFeatureRegistry::Get().InvokeAll<IArithmeticFeature>(
			[](IArithmeticFeature& Feature) { return Feature.AddOne(10); });
		ASSERT_EQ(2u, All.Invocations.size());
		EXPECT_EQ(11, *All.Invocations[0].Value);
		EXPECT_EQ(11, *All.Invocations[1].Value);
	}

	TEST(FModularFeatureTests, MoveResetAndStaleGenerationAreIdentitySafe)
	{

		FArithmeticFeature OldFeature;
		auto OldRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(OldFeature);
		auto MovedRegistration = std::move(OldRegistration);
		EXPECT_FALSE(OldRegistration.IsValid());
		OldRegistration.Reset();
		EXPECT_FALSE(OldRegistration.IsValid());
		MovedRegistration.Reset();
		EXPECT_FALSE(MovedRegistration.IsValid());
		MovedRegistration.Reset();
		EXPECT_FALSE(MovedRegistration.IsValid());

		FArithmeticFeature NewFeature;
		auto NewRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(NewFeature);
		EXPECT_EQ(EFeatureInvokeStatus::Invoked,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature& Feature) {
				return Feature.AddOne(1);
			}).Status);
	}

	TEST(FModularFeatureTests, ResetOnlyRemovesTheSelectedRegistration)
	{

		FArithmeticFeature First;
		FArithmeticFeature Second;
		auto FirstRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(First);
		auto SecondRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Second);
		FirstRegistration.Reset();
		EXPECT_FALSE(FirstRegistration.IsValid());
		EXPECT_EQ(1u, Durin::FModularFeatureRegistry::Get().InvokeAll<IArithmeticFeature>([](auto&) {}).MatchingRegistrationCount);
		EXPECT_EQ(EFeatureInvokeStatus::Invoked,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature&) {}).Status);
		SecondRegistration.Reset();
		EXPECT_FALSE(SecondRegistration.IsValid());
	}

	TEST(FModularFeatureTests, NestedDispatchAndExceptionsRestoreTheSafePoint)
	{

		FArithmeticFeature Feature;
		auto Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Feature);
		auto& Registry = FModularFeatureRegistry::Get();
		EXPECT_FALSE(Registry.IsDispatching());
		EXPECT_TRUE(Registry.InvokeSingle<IArithmeticFeature>([&](auto&) {
			EXPECT_TRUE(Registry.IsDispatching());
			EXPECT_EQ(EFeatureInvokeStatus::VisitorFailed, Registry.InvokeSingle<IArithmeticFeature>([](auto&) -> int {
				throw std::runtime_error("expected nested failure");
			}).Status);
			EXPECT_TRUE(Registry.IsDispatching());
		}).WasInvoked());
		EXPECT_FALSE(Registry.IsDispatching());
		Registration.Reset();
		EXPECT_EQ(0u, Durin::FModularFeatureRegistry::Get().InvokeAll<IArithmeticFeature>([](auto&) {}).MatchingRegistrationCount);
	}

	TEST(FModuleManagerLifecycleTests, FeatureDispatchRejectsReentrantUnloadBeforeShutdown)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"DispatchUnload", std::make_unique<FManagedTestModule>(Observations)));
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<IManagedModuleFeature>([](auto&) {
			return FModuleManager::Get().UnloadModule("DispatchUnload");
		});
		ASSERT_TRUE(Invocation.WasInvoked());
		EXPECT_FALSE(*Invocation.Value);
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
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<IManagedModuleFeature>(
			[](IManagedModuleFeature& Feature) { return Feature.GetValue(); });
		ASSERT_EQ(EFeatureInvokeStatus::Invoked, Invocation.Status);
		EXPECT_EQ(42, *Invocation.Value);

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
