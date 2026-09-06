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
		bool bDestroyed = false;
	};

	class FManagedTestModule final : public IModuleInterface, public IManagedModuleFeature
	{
	public:
		explicit FManagedTestModule(FManagedModuleObservations& InObservations)
			: Observations(InObservations) {}
		~FManagedTestModule() override { Observations.bDestroyed = true; }

		auto StartupModule() -> void override
		{
			Registration = FModuleStartup::RegisterFeature<IManagedModuleFeature>(*this);
		}
		auto ShutdownModule() -> void override { Observations.bShutdown = true; }
		auto GetValue() -> int override { return 42; }

	private:
		FManagedModuleObservations& Observations;
		FModularFeatureRegistration Registration;
	};

	class FFailingShutdownModule final : public IModuleInterface
	{
	public:
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
		EXPECT_TRUE(Registration.Reset().Succeeded());
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
		EXPECT_TRUE(Registration.Reset().Succeeded());
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
		EXPECT_EQ(EModularFeatureRetirementStatus::InvalidRegistration, OldRegistration.Reset().Status);
		EXPECT_TRUE(MovedRegistration.Reset().Succeeded());
		EXPECT_EQ(EModularFeatureRetirementStatus::InvalidRegistration, MovedRegistration.Reset().Status);

		FModuleTestOwner NewContext("FeatureGeneration");
		FArithmeticFeature NewFeature;
		auto NewRegistration = NewContext.RegisterFeature(NewFeature);
		EXPECT_EQ(EFeatureInvokeStatus::Invoked,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature& Feature) {
				return Feature.AddOne(1);
			}).Status);
	}

	TEST(FModularFeatureTests, OwnerRetirementIrreversiblyRejectsLaterRegistration)
	{
		FModuleTestOwner Context("RetiredOwner");
		FArithmeticFeature FirstFeature;
		auto FirstRegistration = Context.RegisterFeature(FirstFeature);
		const auto Retirement = Context.BeginRetirement();
		EXPECT_EQ(0u, Retirement.Snapshot.PublishedCount);

		FArithmeticFeature LateFeature;
		auto LateRegistration = Context.RegisterFeature(LateFeature);
		EXPECT_FALSE(LateRegistration.IsValid());
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature&) {}).Status);
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
		EXPECT_EQ(EModularFeatureRetirementStatus::TimedOut, Registration.Reset(std::chrono::milliseconds(1)).Status);
		{
			std::lock_guard Lock(Mutex);
			bRelease = true;
		}
		CV.notify_all();
		Caller.join();
		const auto Retired = Registration.Reset();
		EXPECT_TRUE(Retired.Succeeded());
		EXPECT_EQ(0u, Retired.Snapshot.PublishedCount);
		EXPECT_EQ(0u, Retired.Snapshot.InFlightInvocationCount);
	}

	TEST(FModularFeatureTests, SelfWaitAndVisitorFailureAreCategorizedWithoutLeakingAdmission)
	{
		FModuleTestOwner Context("FeatureFailures");
		FArithmeticFeature Feature;
		auto Registration = Context.RegisterFeature(Feature);
		EModularFeatureRetirementStatus SelfWaitStatus = EModularFeatureRetirementStatus::Succeeded;
		const auto SelfWait = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([&](IArithmeticFeature&) {
			SelfWaitStatus = Registration.Reset().Status;
		});
		EXPECT_EQ(EFeatureInvokeStatus::Invoked, SelfWait.Status);
		EXPECT_EQ(EModularFeatureRetirementStatus::SelfWait, SelfWaitStatus);
		EXPECT_TRUE(Registration.Reset().Succeeded());

		FModuleTestOwner FailureContext("FeatureVisitorFailure");
		FArithmeticFeature FailureFeature;
		auto FailureRegistration = FailureContext.RegisterFeature(FailureFeature);
		const auto Failure = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature&) -> int {
			throw std::runtime_error("expected test exception");
		});
		EXPECT_EQ(EFeatureInvokeStatus::VisitorFailed, Failure.Status);
		EXPECT_TRUE(FailureRegistration.Reset().Succeeded());
	}

	TEST(FModuleManagerRetirementTests, SuccessfulUnloadRetiresFeaturesBeforeDestroyingModule)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleSuccess", std::make_unique<FManagedTestModule>(Observations)));
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<IManagedModuleFeature>(
			[](IManagedModuleFeature& Feature) { return Feature.GetValue(); });
		ASSERT_EQ(EFeatureInvokeStatus::Invoked, Invocation.Status);
		EXPECT_EQ(42, *Invocation.Value);

		const auto Result = FModuleManager::Get().UnloadModule("ManagedModuleSuccess");
		EXPECT_TRUE(Result.Succeeded()) << Result.Message;
		EXPECT_EQ(EModuleState::Unloaded, Result.ObservedState);
		EXPECT_EQ(0u, Result.RetirementSnapshot.PublishedCount);
		EXPECT_EQ(0u, Result.RetirementSnapshot.InFlightInvocationCount);
		EXPECT_TRUE(Observations.bShutdown);
		EXPECT_TRUE(Observations.bDestroyed);
	}

	TEST(FModuleManagerRetirementTests, ReflectedObjectRejectionFailsClosedAndRetainsInstance)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleReflectedBlock", std::make_unique<FManagedTestModule>(Observations)));
		FModuleManager::Get().SetPreShutdownModuleCallback([](FName) { return false; });
		const auto Result = FModuleManager::Get().UnloadModule("ManagedModuleReflectedBlock");
		FModuleManager::Get().SetPreShutdownModuleCallback({});

		EXPECT_EQ(EModuleOperationStatus::ReflectedObjectDrainRejected, Result.Status);
		EXPECT_EQ(EModuleState::UnloadBlocked, Result.ObservedState);
		EXPECT_FALSE(Observations.bShutdown);
		EXPECT_FALSE(Observations.bDestroyed);
		EXPECT_NE(nullptr, FModuleManager::Get().FindModule("ManagedModuleReflectedBlock")->Module.get());
	}

	TEST(FModuleManagerRetirementTests, SelfUnloadIsRejectedWithoutBlockingOrReleasingTheModule)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleSelfUnload", std::make_unique<FManagedTestModule>(Observations)));
		FModuleUnloadResult UnloadResult;
		const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<IManagedModuleFeature>(
			[&](IManagedModuleFeature&) {
				UnloadResult = FModuleManager::Get().UnloadModule("ManagedModuleSelfUnload");
			});

		EXPECT_EQ(EFeatureInvokeStatus::Invoked, Invocation.Status);
		EXPECT_EQ(EModuleOperationStatus::RecursiveOwnedExecution, UnloadResult.Status);
		EXPECT_EQ(EModuleState::UnloadBlocked, UnloadResult.ObservedState);
		EXPECT_FALSE(Observations.bShutdown);
		EXPECT_FALSE(Observations.bDestroyed);
	}

	TEST(FModuleManagerRetirementTests, WrongThreadDoesNotStartIrreversibleRetirement)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleWrongThread", std::make_unique<FManagedTestModule>(Observations)));
		FModuleShutdownResult WrongThreadResult;
		std::thread Worker([&]() {
			WrongThreadResult = FModuleManager::Get().ShutdownModule("ManagedModuleWrongThread");
		});
		Worker.join();
		EXPECT_EQ(EModuleOperationStatus::WrongControlThread, WrongThreadResult.Status);
		EXPECT_EQ(EModuleState::Active, WrongThreadResult.ObservedState);

		const auto Cleanup = FModuleManager::Get().UnloadModule("ManagedModuleWrongThread");
		EXPECT_TRUE(Cleanup.Succeeded()) << Cleanup.Message;
	}

	TEST(FModuleManagerRetirementTests, InvocationTimeoutFailsClosedAndRetainsInstance)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleTimeout", std::make_unique<FManagedTestModule>(Observations)));
		std::mutex Mutex;
		std::condition_variable CV;
		bool bEntered = false;
		bool bRelease = false;
		std::thread Caller([&]() {
			(void)FModularFeatureRegistry::Get().InvokeSingle<IManagedModuleFeature>([&](IManagedModuleFeature&) {
				std::unique_lock Lock(Mutex);
				bEntered = true;
				CV.notify_all();
				CV.wait(Lock, [&]() { return bRelease; });
			});
		});
		{
			std::unique_lock Lock(Mutex);
			CV.wait(Lock, [&]() { return bEntered; });
		}
		const auto PreviousTimeout = FModuleTestHarness::SetRetirementTimeout(std::chrono::milliseconds(5));
		const auto Result = FModuleManager::Get().UnloadModule("ManagedModuleTimeout");
		(void)FModuleTestHarness::SetRetirementTimeout(PreviousTimeout);
		EXPECT_EQ(EModuleOperationStatus::FeatureInvocationDrainTimeout, Result.Status);
		EXPECT_EQ(EModuleState::UnloadBlocked, Result.ObservedState);
		EXPECT_EQ(1u, Result.RetirementSnapshot.InFlightInvocationCount);
		EXPECT_FALSE(Observations.bDestroyed);
		{
			std::lock_guard Lock(Mutex);
			bRelease = true;
		}
		CV.notify_all();
		Caller.join();
	}

	TEST(FModuleManagerRetirementTests, ShutdownCallbackFailureRetainsMappedInstance)
	{
		bool bDestroyed = false;
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(
			"ManagedModuleShutdownFailure", std::make_unique<FFailingShutdownModule>(bDestroyed)));
		const auto Result = FModuleManager::Get().UnloadModule("ManagedModuleShutdownFailure");
		EXPECT_EQ(EModuleOperationStatus::ShutdownCallbackFailure, Result.Status);
		EXPECT_EQ(EModuleState::UnloadBlocked, Result.ObservedState);
		EXPECT_FALSE(bDestroyed);
	}
}
