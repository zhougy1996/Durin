#include "Modules/ModularFeature.h"
#include "Modules/ModuleTestContext.h"

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

		auto StartupModule(FModuleContext& Context) -> void override
		{
			Registration = Context.RegisterFeature<IManagedModuleFeature>(*this);
		}
		auto ShutdownModule(FModuleShutdownContext&) -> void override { Observations.bShutdown = true; }
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
		auto ShutdownModule(FModuleShutdownContext&) -> void override
		{
			throw std::runtime_error("expected shutdown failure");
		}

	private:
		bool& bDestroyed;
	};

	class FOwnedCallbackTestModule final : public IModuleInterface
	{
	public:
		auto StartupModule(FModuleContext& Context) -> void override
		{
			Registration = Context.CreateOwnedCallbackRegistration("Tests.SpecializedRegistry");
			Gate = Registration.GetGate();
		}

		FModuleOwnedCallbackGate Gate;
	private:
		FModuleOwnedCallbackRegistration Registration;
	};

	TEST(FModularFeatureTests, UsesDeclaredNameAndVersionAndRejectsInvalidIdentity)
	{
		auto Context = FModuleTestContextFactory::CreateStartupContext("FeatureIdentityTest");
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

	TEST(FModularFeatureTests, ReportsUnavailableAmbiguousAndInvokesPinnedSet)
	{
		auto FirstContext = FModuleTestContextFactory::CreateStartupContext("FeatureCardinalityA");
		auto SecondContext = FModuleTestContextFactory::CreateStartupContext("FeatureCardinalityB");
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
		auto OldContext = FModuleTestContextFactory::CreateStartupContext("FeatureGeneration");
		FArithmeticFeature OldFeature;
		auto OldRegistration = OldContext.RegisterFeature(OldFeature);
		auto MovedRegistration = std::move(OldRegistration);
		EXPECT_FALSE(OldRegistration.IsValid());
		EXPECT_EQ(EModularFeatureRetirementStatus::InvalidRegistration, OldRegistration.Reset().Status);
		EXPECT_TRUE(MovedRegistration.Reset().Succeeded());
		EXPECT_EQ(EModularFeatureRetirementStatus::InvalidRegistration, MovedRegistration.Reset().Status);

		auto NewContext = FModuleTestContextFactory::CreateStartupContext("FeatureGeneration");
		FArithmeticFeature NewFeature;
		auto NewRegistration = NewContext.RegisterFeature(NewFeature);
		EXPECT_EQ(EFeatureInvokeStatus::Invoked,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature& Feature) {
				return Feature.AddOne(1);
			}).Status);
	}

	TEST(FModularFeatureTests, OwnerRetirementIrreversiblyRejectsLaterRegistration)
	{
		auto Context = FModuleTestContextFactory::CreateStartupContext("RetiredOwner");
		FArithmeticFeature FirstFeature;
		auto FirstRegistration = Context.RegisterFeature(FirstFeature);
		auto ShutdownContext = FModuleTestContextFactory::CreateShutdownContext(Context);
		EXPECT_EQ(0u, ShutdownContext.GetFeatureRetirementSnapshot().PublishedCount);

		FArithmeticFeature LateFeature;
		auto LateRegistration = Context.RegisterFeature(LateFeature);
		EXPECT_FALSE(LateRegistration.IsValid());
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable,
			FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature&) {}).Status);
	}

	TEST(FModularFeatureTests, RetirementClosesAdmissionAndWaitsForEnteredInvocation)
	{
		auto Context = FModuleTestContextFactory::CreateStartupContext("FeatureRace");
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
		auto Context = FModuleTestContextFactory::CreateStartupContext("FeatureFailures");
		FArithmeticFeature Feature;
		auto Registration = Context.RegisterFeature(Feature);
		EModularFeatureRetirementStatus SelfWaitStatus = EModularFeatureRetirementStatus::Succeeded;
		const auto SelfWait = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([&](IArithmeticFeature&) {
			SelfWaitStatus = Registration.Reset().Status;
		});
		EXPECT_EQ(EFeatureInvokeStatus::Invoked, SelfWait.Status);
		EXPECT_EQ(EModularFeatureRetirementStatus::SelfWait, SelfWaitStatus);
		EXPECT_TRUE(Registration.Reset().Succeeded());

		auto FailureContext = FModuleTestContextFactory::CreateStartupContext("FeatureVisitorFailure");
		FArithmeticFeature FailureFeature;
		auto FailureRegistration = FailureContext.RegisterFeature(FailureFeature);
		const auto Failure = FModularFeatureRegistry::Get().InvokeSingle<IArithmeticFeature>([](IArithmeticFeature&) -> int {
			throw std::runtime_error("expected test exception");
		});
		EXPECT_EQ(EFeatureInvokeStatus::VisitorFailed, Failure.Status);
		EXPECT_TRUE(FailureRegistration.Reset().Succeeded());
	}

	TEST(FModularFeatureTests, OwnedCallbackGateRetiresCallsAndAuditsResources)
	{
		auto Context = FModuleTestContextFactory::CreateStartupContext("OwnedCallbackGate");
		auto Registration = Context.CreateOwnedCallbackRegistration("Tests.Registry");
		ASSERT_TRUE(Registration.IsValid());
		const FModuleOwnedCallbackGate Gate = Registration.GetGate();
		{
			auto Invocation = Gate.TryEnter();
			ASSERT_TRUE(Invocation);
			const auto Retiring = Registration.Retire();
			EXPECT_EQ(1u, Retiring.InFlightInvocationCount);
			EXPECT_FALSE(Gate.TryEnter());
		}
		EXPECT_TRUE(Registration.Reset().Succeeded());

		auto ResourceContext = FModuleTestContextFactory::CreateStartupContext("OwnedCallbackResource");
		auto ResourceRegistration = ResourceContext.CreateOwnedCallbackRegistration("Tests.Registry");
		const auto ResourceGate = ResourceRegistration.GetGate();
		auto Resource = ResourceGate.RetainResource();
		ASSERT_TRUE(Resource);
		const auto Retiring = ResourceRegistration.Retire();
		EXPECT_EQ(1u, Retiring.RetainedResourceCount);
		EXPECT_FALSE(ResourceGate.RetainResource());
		EXPECT_EQ(EModularFeatureRetirementStatus::TimedOut,
			ResourceRegistration.Reset(std::chrono::milliseconds(1)).Status);
		Resource = {};
		EXPECT_TRUE(ResourceRegistration.Reset().Succeeded());
	}

	TEST(FModuleManagerRetirementTests, RetainedOwnedResourceFailsClosedBeforeModuleDestruction)
	{
		auto Module = std::make_unique<FOwnedCallbackTestModule>();
		auto* ModulePointer = Module.get();
		ASSERT_NE(nullptr, FModuleTestContextFactory::InstallStartedModule(
			"ManagedModuleRetainedResource", std::move(Module)));
		auto Resource = ModulePointer->Gate.RetainResource();
		ASSERT_TRUE(Resource);
		const auto Result = FModuleManager::Get().UnloadModule("ManagedModuleRetainedResource");
		EXPECT_EQ(EModuleOperationStatus::OutstandingFeatureAudit, Result.Status);
		EXPECT_EQ(1u, Result.RetirementSnapshot.RetainedResourceCount);
		EXPECT_NE(nullptr, FModuleManager::Get().FindModule(
			"ManagedModuleRetainedResource")->Module.get());
	}

	TEST(FModuleManagerRetirementTests, SuccessfulUnloadRetiresFeaturesBeforeDestroyingModule)
	{
		FManagedModuleObservations Observations;
		ASSERT_NE(nullptr, FModuleTestContextFactory::InstallStartedModule(
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
		ASSERT_NE(nullptr, FModuleTestContextFactory::InstallStartedModule(
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
		ASSERT_NE(nullptr, FModuleTestContextFactory::InstallStartedModule(
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
		ASSERT_NE(nullptr, FModuleTestContextFactory::InstallStartedModule(
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
		ASSERT_NE(nullptr, FModuleTestContextFactory::InstallStartedModule(
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
		const auto PreviousTimeout = FModuleTestContextFactory::SetRetirementTimeout(std::chrono::milliseconds(5));
		const auto Result = FModuleManager::Get().UnloadModule("ManagedModuleTimeout");
		(void)FModuleTestContextFactory::SetRetirementTimeout(PreviousTimeout);
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
		ASSERT_NE(nullptr, FModuleTestContextFactory::InstallStartedModule(
			"ManagedModuleShutdownFailure", std::make_unique<FFailingShutdownModule>(bDestroyed)));
		const auto Result = FModuleManager::Get().UnloadModule("ManagedModuleShutdownFailure");
		EXPECT_EQ(EModuleOperationStatus::ShutdownCallbackFailure, Result.Status);
		EXPECT_EQ(EModuleState::UnloadBlocked, Result.ObservedState);
		EXPECT_FALSE(bDestroyed);
	}
}
