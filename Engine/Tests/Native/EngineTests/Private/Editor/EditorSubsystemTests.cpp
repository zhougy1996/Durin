#include "Editor/EditorEngine.h"
#include "Editor/EditorSubsystem.h"
#include "Tools/EditorToolSubsystem.h"
#include "DObject/Class.h"
#include "DObject/StrongObjectPtr.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include <gtest/gtest.h>

namespace Durin
{
	class FDynamicEditorHost : public DEditorEngine
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FDynamicEditorHost, DEditorEngine, FDynamicEditorHost::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FDynamicEditorHost)
	public:
		explicit FDynamicEditorHost(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
		auto Start() -> FSubsystemResult { return InitializeEditorSubsystems(); }
	};
	class FDynamicEditorA : public DEditorSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FDynamicEditorA, DEditorSubsystem, FDynamicEditorA::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FDynamicEditorA)
	public:
		explicit FDynamicEditorA(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
		auto Initialize() -> FSubsystemResult override { return Init ? Init(*this) : FSubsystemResult{}; }
		auto Deinitialize() noexcept -> void override { if (Deinit) Deinit(*this); }
		auto BeginDestroy() -> void override { if (Destroy) Destroy(*this); Super::BeginDestroy(); }
		inline static std::function<FSubsystemResult(FDynamicEditorA&)> Init;
		inline static std::function<void(FDynamicEditorA&)> Deinit;
		inline static std::function<void(FDynamicEditorA&)> Destroy;
	};
	class FDynamicEditorB : public FDynamicEditorA
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FDynamicEditorB, FDynamicEditorA, FDynamicEditorB::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FDynamicEditorB)
	public:
		explicit FDynamicEditorB(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FDynamicEditorHost)
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FDynamicEditorA)
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FDynamicEditorB)
}

namespace
{
	using namespace Durin;
	class FEditorSubsystemTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override { Testing::InitializeDObjectSystemForTests(); }
		auto TearDown() -> void override
		{
			FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {}; FDynamicEditorA::Destroy = {};
			for (auto& Host : Hosts) { Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get()); }
			Hosts.clear(); CollectGarbage();
		}
		auto NewHost() -> FDynamicEditorHost*
		{
			auto* Host = NewObject<FDynamicEditorHost>(nullptr, {}); Hosts.emplace_back(Host);
			EXPECT_TRUE(Host->Start()); return Host;
		}
		auto RegisterA() -> std::unique_ptr<FEditorSubsystemRegistration>
		{ return std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorA::StaticClass()}); }
		std::vector<TStrongObjectPtr<FDynamicEditorHost>> Hosts;
	};

	TEST_F(FEditorSubsystemTests, AddsAndRemovesPerHostInstancesWithoutRetiringTheHost)
	{
		auto* First = NewHost(); auto* Second = NewHost();
		auto* Tools = First->GetEditorSubsystem<DEditorToolSubsystem>();
		auto Registration = RegisterA();
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorA>());
		ASSERT_TRUE(First->RefreshEditorSubsystems()); ASSERT_TRUE(Second->RefreshEditorSubsystems());
		auto* A = First->GetEditorSubsystem<FDynamicEditorA>(); ASSERT_NE(nullptr, A);
		EXPECT_NE(A, Second->GetEditorSubsystem<FDynamicEditorA>());
		auto Gate = A->GetWorkGate(); Registration.reset();
		EXPECT_TRUE(Gate->IsOpen());
		ASSERT_TRUE(First->RefreshEditorSubsystems()); EXPECT_FALSE(Gate->IsOpen());
		ASSERT_TRUE(Second->RefreshEditorSubsystems());
		FDynamicEditorA::Destroy = [&](auto&) {
			EXPECT_TRUE(IsGarbageCollecting());
			EXPECT_EQ(ESubsystemError::InvalidState, First->RefreshEditorSubsystems().Error);
		};
		CollectGarbage();
		FDynamicEditorA::Destroy = {};
		EXPECT_EQ(ESubsystemState::Ready, First->GetEditorSubsystemState());
		EXPECT_EQ(Tools, First->GetEditorSubsystem<DEditorToolSubsystem>());
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorA>());
	}

	TEST_F(FEditorSubsystemTests, ResolvesLateDependenciesAndRetiresDependentsFirst)
	{
		auto* Host = NewHost(); std::vector<std::string> Events;
		FDynamicEditorA::Init = [&](auto& Object) {
			Events.push_back(Object.GetClass() == FDynamicEditorA::StaticClass() ? "A+" : "B+"); return FSubsystemResult{};
		};
		FDynamicEditorA::Deinit = [&](auto& Object) {
			Events.push_back(Object.GetClass() == FDynamicEditorA::StaticClass() ? "A-" : "B-");
			if (Object.GetClass() == FDynamicEditorB::StaticClass()) EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		};
		auto B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{
			.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		EXPECT_EQ(ESubsystemError::MissingDependency, Host->RefreshEditorSubsystems().Error);
		auto A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		EXPECT_EQ((std::vector<std::string>{"A+", "B+"}), Events);
		A.reset(); EXPECT_EQ(ESubsystemError::MissingDependency, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ((std::vector<std::string>{"A+", "B+", "B-", "A-"}), Events);
		A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		ASSERT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		// A replacement also rebuilds a dependent whose own token did not change.
		auto OldGate = Host->GetEditorSubsystem<FDynamicEditorB>()->GetWorkGate();
		A.reset(); A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		EXPECT_FALSE(OldGate->IsOpen());
		FDynamicEditorA::Init = {}; FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, RejectsDuplicateTypesAndCyclesWithoutLosingUnrelatedServices)
	{
		auto* Host = NewHost(); auto* Tools = Host->GetEditorSubsystem<DEditorToolSubsystem>();
		auto A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		auto Duplicate = RegisterA(); EXPECT_EQ(ESubsystemError::DuplicateType, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		Duplicate.reset(); ASSERT_TRUE(Host->RefreshEditorSubsystems()); A.reset();
		FEditorSubsystemRegistration CycleA({.Type = FDynamicEditorA::StaticClass(), .Dependencies = {FDynamicEditorB::StaticClass()}});
		FEditorSubsystemRegistration CycleB({.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		EXPECT_EQ(ESubsystemError::DependencyCycle, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(Tools, Host->GetEditorSubsystem<DEditorToolSubsystem>());
	}

	TEST_F(FEditorSubsystemTests, RollsBackFailedAdditionAndRetriesOnlyWhenRequested)
	{
		auto* Host = NewHost(); int Initializations = 0, Cleanups = 0;
		auto A = RegisterA(); FEditorSubsystemRegistration B({.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		FDynamicEditorA::Init = [&](auto& Object) {
			++Initializations;
			return Object.GetClass() == FDynamicEditorB::StaticClass()
				? FSubsystemResult{ESubsystemError::InitializationFailed, "expected failure"} : FSubsystemResult{};
		};
		FDynamicEditorA::Deinit = [&](auto&) { ++Cleanups; };
		EXPECT_EQ(ESubsystemError::InitializationFailed, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(2, Cleanups); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		(void)Host->RefreshEditorSubsystems(); EXPECT_EQ(2, Initializations);
		FDynamicEditorA::Init = {}; ASSERT_TRUE(Host->RefreshEditorSubsystems(true));
		FDynamicEditorA::Deinit = {};
	}

	TEST_F(FEditorSubsystemTests, DefersChangesFromCallbacksAndInitialization)
	{
		auto* Host = NewHost(); std::unique_ptr<FEditorSubsystemRegistration> B;
		auto A = RegisterA();
		FDynamicEditorA::Init = [&](auto& Object) {
			if (Object.GetClass() == FDynamicEditorA::StaticClass())
			{
				B = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorB::StaticClass()});
				EXPECT_EQ(ESubsystemError::InvalidState, Host->RefreshEditorSubsystems().Error);
				EXPECT_FALSE(Host->GetEditorSubsystem<DEditorToolSubsystem>()->CanCallNow());
			}
			return FSubsystemResult{};
		};
		ASSERT_TRUE(Host->RefreshEditorSubsystems()); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		ASSERT_TRUE(Host->RefreshEditorSubsystems()); ASSERT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorB>());
		Host->DispatchSubsystemCallback([&] {
			A.reset(); EXPECT_EQ(ESubsystemError::InvalidState, Host->RefreshEditorSubsystems().Error);
			EXPECT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		});
		ASSERT_TRUE(Host->RefreshEditorSubsystems()); EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		FDynamicEditorA::Init = {};
	}
	TEST_F(FEditorSubsystemTests, ExceptionsAndHostRetirementRollBackNewInstances)
	{
		auto* Host = NewHost(); auto A = RegisterA();
		FDynamicEditorA::Init = [](auto&) -> FSubsystemResult { throw std::runtime_error("expected"); };
		EXPECT_EQ(ESubsystemError::InitializationFailed, Host->RefreshEditorSubsystems().Error);
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		FDynamicEditorA::Init = [&](auto&) { Host->PrepareForShutdown(); return FSubsystemResult{}; };
		EXPECT_EQ(ESubsystemError::Aborted, Host->RefreshEditorSubsystems(true).Error);
		EXPECT_EQ(ESubsystemState::Shutdown, Host->GetEditorSubsystemState());
		FDynamicEditorA::Init = {};
	}
	TEST_F(FEditorSubsystemTests, ClosingAGateDoesNotBypassUnfinishedHostRetirement)
	{
		auto* Host = NewHost(); auto A = RegisterA(); ASSERT_TRUE(Host->RefreshEditorSubsystems());
		auto* Service = Host->GetEditorSubsystem<FDynamicEditorA>(); ASSERT_NE(nullptr, Service);
		Host->DispatchSubsystemCallback([&] {
			Host->PrepareForShutdown();
			EXPECT_FALSE(Service->GetWorkGate()->IsOpen());
			EXPECT_FALSE(Service->IsReadyForFinishDestroy());
		});
		Host->PrepareForShutdown();
		EXPECT_TRUE(Service->IsReadyForFinishDestroy());
	}

	class FDynamicProvider : public IModuleInterface
	{
	public:
		explicit FDynamicProvider(std::shared_ptr<int> InStopped) : Stopped(std::move(InStopped)) {}
		auto SupportsDynamicReloading() const -> bool override { return true; }
		auto StartupModule() -> void override
		{
			Registration = std::make_unique<FEditorSubsystemRegistration>(FSubsystemDescriptor{.Type = FDynamicEditorA::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
			if (DuringStartup) DuringStartup();
		}
		auto CanShutdownModule() const -> bool override { return FEditorSubsystemCollection::CanSynchronizeAll(); }
		auto ShutdownModule() -> void override
		{
			require(CanShutdownModule());
			Registration.reset();
			FEditorSubsystemCollection::SynchronizeAll();
			++*Stopped;
		}
		std::unique_ptr<FEditorSubsystemRegistration> Registration;
		std::shared_ptr<int> Stopped;
		std::function<void()> DuringStartup;
	};

	TEST_F(FEditorSubsystemTests, ModuleShutdownUnregistersServicesBeforeUnload)
	{
		auto* Host = NewHost(); auto Stopped = std::make_shared<int>(0);
		const FName Name("DynamicEditorShutdownTest");
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::make_unique<FDynamicProvider>(Stopped)));
		ASSERT_TRUE(Host->RefreshEditorSubsystems());
		TStrongObjectPtr<FDynamicEditorA> Object(Host->GetEditorSubsystem<FDynamicEditorA>());
		auto Gate = Object->GetWorkGate();
		Host->DispatchSubsystemCallback([&] {
			EXPECT_FALSE(FModuleManager::Get().UnloadModule(Name)); EXPECT_TRUE(Gate->IsOpen());
			EXPECT_EQ(0, *Stopped);
			EXPECT_EQ(EModuleState::Active, FModuleManager::Get().FindModule(Name)->State.load());
		});
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name)); EXPECT_FALSE(Gate->IsOpen());
		EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>()); EXPECT_EQ(1, *Stopped);
		EXPECT_EQ(EModuleState::Unloaded, FModuleManager::Get().FindModule(Name)->State.load());
		// Fixture object code is resident in this test executable; reflected DLLs do not opt into unload.
		Object.Reset(); CollectGarbage();
		EXPECT_FALSE(Gate->IsOpen()); EXPECT_EQ(1, *Stopped);
		EXPECT_EQ(ESubsystemState::Ready, Host->GetEditorSubsystemState());
	}
	TEST_F(FEditorSubsystemTests, ExplicitModuleShutdownRemovesDependenciesAcrossHostsBeforeReturning)
	{
		auto* First = NewHost(); auto* Second = NewHost();
		const FName Name("DynamicEditorExplicitShutdownTest"); auto Stopped = std::make_shared<int>(0);
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::make_unique<FDynamicProvider>(Stopped)));
		FEditorSubsystemRegistration B({.Type = FDynamicEditorB::StaticClass(), .Dependencies = {FDynamicEditorA::StaticClass()}});
		ASSERT_TRUE(First->RefreshEditorSubsystems()); ASSERT_TRUE(Second->RefreshEditorSubsystems());
		auto FirstGate = First->GetEditorSubsystem<FDynamicEditorB>()->GetWorkGate();
		auto SecondGate = Second->GetEditorSubsystem<FDynamicEditorB>()->GetWorkGate();
		Second->DispatchSubsystemCallback([&] {
			EXPECT_FALSE(FModuleManager::Get().UnloadModule(Name));
			EXPECT_TRUE(FirstGate->IsOpen()); EXPECT_TRUE(SecondGate->IsOpen());
		});
		FModuleManager::Get().ShutdownModule(Name);
		EXPECT_EQ(1, *Stopped); EXPECT_FALSE(FirstGate->IsOpen()); EXPECT_FALSE(SecondGate->IsOpen());
		EXPECT_EQ(nullptr, First->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_EQ(nullptr, Second->GetEditorSubsystem<FDynamicEditorB>());
		CollectGarbage(); EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name));
	}
	TEST_F(FEditorSubsystemTests, DoesNotInstantiateAProviderUntilItsModuleStartupCompletes)
	{
		auto* Host = NewHost(); const FName Name("DynamicEditorStartupTest");
		auto Provider = std::make_unique<FDynamicProvider>(std::make_shared<int>(0));
		Provider->DuringStartup = [&] {
			EXPECT_TRUE(Host->RefreshEditorSubsystems());
			EXPECT_EQ(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		};
		ASSERT_NE(nullptr, FModuleTestHarness::InstallStartedModule(Name, std::move(Provider)));
		ASSERT_TRUE(Host->RefreshEditorSubsystems()); ASSERT_NE(nullptr, Host->GetEditorSubsystem<FDynamicEditorA>());
		EXPECT_TRUE(FModuleManager::Get().UnloadModule(Name)); CollectGarbage();
	}
}
