#include "Modules/ModularFeature.h"
#include "Modules/ModuleTestSupport.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <mutex>
#include <thread>

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
			Registration = FModuleStartup::RegisterFeature<IManagedModuleFeature>(*this);
		}
		auto ShutdownModule() -> void override
		{
			// The manager must leave the registration available until our callback.
			EXPECT_EQ(EFeatureInvokeStatus::Invoked,
				FModularFeatureRegistry::Get().InvokeSingle<IManagedModuleFeature>([](auto&) {}).Status);
			require(Registration.Reset() == EModularFeatureRetirementStatus::Succeeded);
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

	TEST(FModuleStartupTests, OwnerCreationOutsideStartupIsRejected)
	{
		EXPECT_DEATH(
			(void)FModuleStartup::CreateAsyncOperationGroup(
				"Tests.OutsideModuleStartup"),
			".*");
	}

	TEST(FModularFeatureTests, UsesDeclaredNameAndVersionAndRejectsInvalidIdentity)
	{
		FModuleTestOwner Context("FeatureIdentityTest");
		FArithmeticFeature V1;
		FArithmeticFeatureV2 V2;
		FInvalidFeature Invalid;
		auto V1Registration = Context.RegisterFeature(V1);
		auto V2Registration = Context.RegisterFeature(V2);
		auto InvalidRegistration = Context.RegisterFeature(Invalid);

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
		FModuleTestOwner Context("FeatureExactRegistration");
		FArithmeticFeature Feature;
		auto Registration = Context.RegisterFeature(Feature);
		const auto First = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>(
			[](IArithmeticFeature& Value) { return Value.AddOne(1); });
		ASSERT_TRUE(First.WasInvoked());
		ASSERT_NE(0u, First.RegistrationIdentity);
		EXPECT_TRUE(Registration.Reset() == EModularFeatureRetirementStatus::Succeeded);
		Registration = Context.RegisterFeature(Feature);
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
		EXPECT_TRUE(Registration.Reset() == EModularFeatureRetirementStatus::Succeeded);
	}

	TEST(FModularFeatureTests, ReportsUnavailableAmbiguousAndInvokesPinnedSet)
	{
		FModuleTestOwner FirstContext("FeatureCardinalityA");
		FModuleTestOwner SecondContext("FeatureCardinalityB");
		FArithmeticFeature First;
		FArithmeticFeature Second;

		const auto Unavailable = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>(
			[](IArithmeticFeature& Feature) { return Feature.AddOne(0); });
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable, Unavailable.Status);

		auto FirstRegistration = FirstContext.RegisterFeature(First);
		auto SecondRegistration = SecondContext.RegisterFeature(Second);
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
		FModuleTestOwner OldContext("FeatureGeneration");
		FArithmeticFeature OldFeature;
		auto OldRegistration = OldContext.RegisterFeature(OldFeature);
		auto MovedRegistration = std::move(OldRegistration);
		EXPECT_FALSE(OldRegistration.IsValid());
		EXPECT_EQ(EModularFeatureRetirementStatus::InvalidRegistration, OldRegistration.Reset());
		EXPECT_TRUE(MovedRegistration.Reset() == EModularFeatureRetirementStatus::Succeeded);
		EXPECT_EQ(EModularFeatureRetirementStatus::InvalidRegistration, MovedRegistration.Reset());

		FModuleTestOwner NewContext("FeatureGeneration");
		FArithmeticFeature NewFeature;
		auto NewRegistration = NewContext.RegisterFeature(NewFeature);
		EXPECT_EQ(EFeatureInvokeStatus::Invoked,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature& Feature) {
				return Feature.AddOne(1);
			}).Status);
	}

	TEST(FModularFeatureTests, ResetOnlyRemovesTheSelectedRegistration)
	{
		FModuleTestOwner Context("LocalRegistrationOwnership");
		FArithmeticFeature First;
		FArithmeticFeature Second;
		auto FirstRegistration = Context.RegisterFeature(First);
		auto SecondRegistration = Context.RegisterFeature(Second);
		EXPECT_EQ(EModularFeatureRetirementStatus::Succeeded, FirstRegistration.Reset());
		EXPECT_EQ(1u, Context.GetFeatureSnapshot().PublishedCount);
		EXPECT_EQ(EFeatureInvokeStatus::Invoked,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature&) {}).Status);
		EXPECT_EQ(EModularFeatureRetirementStatus::Succeeded, SecondRegistration.Reset());
	}

	TEST(FModularFeatureTests, RetirementClosesAdmissionAndWaitsForEnteredInvocation)
	{
		FModuleTestOwner Context("FeatureRace");
		FArithmeticFeature Feature;
		auto Registration = Context.RegisterFeature(Feature);
		std::mutex Mutex;
		std::condition_variable CV;
		bool bEntered = false;
		bool bRelease = false;

		std::thread Caller([&]() {
			const auto Result = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([&](IArithmeticFeature&) {
				std::unique_lock Lock(Mutex);
				bEntered = true;
				CV.notify_all();
				CV.wait(Lock, [&]() { return bRelease; });
			});
			EXPECT_EQ(EFeatureInvokeStatus::Invoked, Result.Status);
		});
		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [&]() { return bEntered; });
		}

		const auto Retiring = Registration.Retire();
		EXPECT_EQ(0u, Retiring.PublishedCount);
		EXPECT_EQ(1u, Retiring.InFlightInvocationCount);
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature&) {}).Status);
		EXPECT_EQ(EModularFeatureRetirementStatus::TimedOut, Registration.Reset(std::chrono::milliseconds(1)));
		{
			std::lock_guard Lock(Mutex);
			bRelease = true;
		}
		CV.notify_all();
		Caller.join();
		const auto Retired = Registration.Reset();
		EXPECT_EQ(EModularFeatureRetirementStatus::Succeeded, Retired);
		EXPECT_EQ(0u, Context.GetFeatureSnapshot().PublishedCount);
		EXPECT_EQ(0u, Context.GetFeatureSnapshot().InFlightInvocationCount);
	}

	TEST(FModularFeatureTests, SelfWaitAndVisitorFailureAreCategorizedWithoutLeakingAdmission)
	{
		FModuleTestOwner Context("FeatureFailures");
		FArithmeticFeature Feature;
		auto Registration = Context.RegisterFeature(Feature);
		EModularFeatureRetirementStatus SelfWaitStatus = EModularFeatureRetirementStatus::Succeeded;
		const auto SelfWait = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([&](IArithmeticFeature&) {
			SelfWaitStatus = Registration.Reset();
		});
		EXPECT_EQ(EFeatureInvokeStatus::Invoked, SelfWait.Status);
		EXPECT_EQ(EModularFeatureRetirementStatus::SelfWait, SelfWaitStatus);
		EXPECT_TRUE(Registration.Reset() == EModularFeatureRetirementStatus::Succeeded);

		FModuleTestOwner FailureContext("FeatureVisitorFailure");
		FArithmeticFeature FailureFeature;
		auto FailureRegistration = FailureContext.RegisterFeature(FailureFeature);
		const auto Failure = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature&) -> int {
			throw std::runtime_error("expected test exception");
		});
		EXPECT_EQ(EFeatureInvokeStatus::VisitorFailed, Failure.Status);
		EXPECT_TRUE(FailureRegistration.Reset() == EModularFeatureRetirementStatus::Succeeded);
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

	TEST(FModuleManagerLifecycleTests, LiveCodeLeaseRejectsUnloadBeforeShutdown)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleLease", std::make_unique<FManagedTestModule>(Observations)));
		auto& Manager = FModuleManager::Get();
		auto Lease = Manager.AcquireCodeLease("ManagedModuleLease");
		ASSERT_NE(nullptr, Lease);
		EXPECT_FALSE(Manager.UnloadModule("ManagedModuleLease"));
		EXPECT_FALSE(Observations.bShutdown);
		EXPECT_TRUE(Manager.IsModuleLoaded("ManagedModuleLease"));
		Lease.reset();
		EXPECT_TRUE(Manager.UnloadModule("ManagedModuleLease"));
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
