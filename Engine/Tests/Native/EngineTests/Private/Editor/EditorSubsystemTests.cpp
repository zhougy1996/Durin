#include "Editor/EditorEngine.h"
#include "Editor/EditorSubsystem.h"
#include "Tools/EditorToolSubsystem.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Workspace/WorkspaceManager.h"
#include "DObject/Package.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
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

	class FTestAssetEditorInstance final : public IAssetEditorInstance
	{
	public:
		auto GetEditorName() const -> FName override { return "TestAssetEditor"; }
		auto IsPrimaryEditor() const -> bool override { return bPrimary; }
		auto FocusWindow(DObject*) -> void override { ++FocusCount; if (Focus) Focus(); }
		auto GetLastActivationTime() -> double override { return 0.0; }
		auto CloseWindow(EAssetEditorCloseReason Reason) -> bool override
		{ LastCloseReason = Reason; ++CloseCount; return Close ? Close() : false; }
		EAssetEditorCloseReason LastCloseReason = EAssetEditorCloseReason::AssetEditorHostClosed;
		bool bPrimary = true;
		int FocusCount = 0;
		int CloseCount = 0;
		std::function<void()> Focus;
		std::function<bool()> Close;
	};

	TEST_F(FEditorSubsystemTests, AssetEditorsTrackManyToManyAssociationsAndFocusThePrimary)
	{
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		ASSERT_NE(nullptr, Service);
		auto* Other = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* First = NewObject<DObject>(Host, "FirstAsset");
		auto* Second = NewObject<DObject>(Host, "SecondAsset");
		FTestAssetEditorInstance Primary, Secondary;
		Secondary.bPrimary = false;
		Service->NotifyAssetOpened(First, &Secondary);
		Service->NotifyAssetsOpened({First, Second, First}, &Primary);
		EXPECT_EQ(2u, Service->FindEditorsForAsset(First).size());
		EXPECT_EQ(2u, Service->GetAllEditedAssets().size());
		EXPECT_EQ(2u, Service->GetAllOpenEditors().size());
		EXPECT_TRUE(Other->GetAllOpenEditors().empty());
		EXPECT_EQ(&Primary, Service->FindEditorForAsset(First, true));
		EXPECT_EQ(1, Primary.FocusCount);
		EXPECT_EQ(0, Secondary.FocusCount);
		EXPECT_TRUE(Service->OpenEditorForAsset(First));
		EXPECT_EQ(2, Primary.FocusCount);
		Primary.Focus = [&] { EXPECT_FALSE(Service->OpenEditorForAsset(First)); };
		EXPECT_TRUE(Service->OpenEditorForAsset(First));
		Service->NotifyAssetClosed(First, &Primary);
		EXPECT_EQ(nullptr, Service->FindEditorForAsset(First, false));
		EXPECT_EQ(&Primary, Service->FindEditorForAsset(Second, false));
		Service->NotifyEditorClosed(&Primary);
		Service->NotifyEditorClosed(&Secondary);
		EXPECT_TRUE(Service->GetAllEditedAssets().empty());
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}

	TEST_F(FEditorSubsystemTests, AssetEditorCallsRejectWorkersAndCloseOtherEditorsPreservesTheException)
	{
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Asset = NewObject<DObject>(Host, "Asset");
		FTestAssetEditorInstance Kept, Closed;
		Service->NotifyAssetOpened(Asset, &Kept);
		Service->NotifyAssetOpened(Asset, &Closed);
		std::thread Worker([&] {
			EXPECT_FALSE(Service->OpenEditorForAsset(Asset));
			EXPECT_TRUE(Service->FindEditorsForAsset(Asset).empty());
			EXPECT_FALSE(Service->CloseAllAssetEditors());
			Service->NotifyEditorClosed(&Kept);
		});
		Worker.join();
		Closed.Close = [&] { Service->NotifyEditorClosed(&Closed); return true; };
		Service->CloseOtherEditors(Asset, &Kept);
		EXPECT_EQ(0, Kept.CloseCount);
		EXPECT_EQ(1, Closed.CloseCount);
		EXPECT_EQ(EAssetEditorCloseReason::CloseOtherEditors, Closed.LastCloseReason);
		EXPECT_EQ(&Kept, Service->FindEditorForAsset(Asset, false));
		Service->NotifyEditorClosed(&Kept);
	}

	TEST_F(FEditorSubsystemTests, AssetEditorCloseHonorsVetoAndToleratesRegistryMutation)
	{
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Asset = NewObject<DObject>(Host, "Asset");
		FTestAssetEditorInstance First, Second;
		Service->NotifyAssetsOpened({Asset}, &First);
		Service->NotifyAssetsOpened({Asset}, &Second);
		EXPECT_FALSE(Service->CloseAllAssetEditors());
		EXPECT_EQ(EAssetEditorCloseReason::CloseAllAssetEditors, First.LastCloseReason);
		EXPECT_EQ(0, Service->CloseAllEditorsForAsset(Asset));
		EXPECT_EQ(EAssetEditorCloseReason::CloseAllEditorsForAsset, First.LastCloseReason);
		First.Close = [&] {
			Service->NotifyEditorClosed(&First);
			Service->NotifyEditorClosed(&Second);
			return true;
		};
		const int PreviousSecondCloses = Second.CloseCount;
		EXPECT_EQ(1, Service->CloseAllEditorsForAsset(Asset));
		EXPECT_EQ(PreviousSecondCloses, Second.CloseCount);
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
	}

	TEST_F(FEditorSubsystemTests, AssetEditorCallbacksKeepHostAliveUntilRetirementBoundary)
	{
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		auto* Asset = NewObject<DObject>(Host, "Asset");
		FTestAssetEditorInstance Instance;
		Service->NotifyAssetOpened(Asset, &Instance);
		Instance.Focus = [&] {
			Host->PrepareForShutdown();
			EXPECT_EQ(ESubsystemState::Ready, Host->GetEditorSubsystemState());
			EXPECT_FALSE(Service->GetWorkGate()->IsOpen());
		};
		EXPECT_EQ(nullptr, Service->FindEditorForAsset(Asset, true));
		EXPECT_FALSE(Service->OpenEditorForAsset(Asset));
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
		Service->NotifyEditorClosed(&Instance);
	}

	class FAssetEditorWorkspace final : public Editor::IWorkspace
	{
	public:
		auto GetWorkspaceType() const -> const Editor::FWorkspaceTypeId& override { return Type; }
		auto OpenDocument(const Editor::FDocumentTab& Document) -> Editor::EDocumentOpenResult override
		{ LastRequestedId = Document.Id; ++OpenCount; return OpenResult; }
		auto ActivateDocument(const Editor::FDocumentTab&) -> void override { ++FocusCount; }
		auto RequestCloseDocument(const Editor::FDocumentTab&) -> Editor::EDocumentCloseResult override
		{ return bDirty ? Editor::EDocumentCloseResult::PendingConfirmation : Editor::EDocumentCloseResult::Closed; }
		auto DiscardDocument(const Editor::FDocumentTab&) -> bool override { bDirty = false; return true; }
		auto DrawWorkspace(bool) -> bool override { return true; }
		auto ResetLayout() -> void override {}
		Editor::FWorkspaceTypeId Type{"AssetEditorTest"};
		Editor::EDocumentOpenResult OpenResult = Editor::EDocumentOpenResult::Opened;
		Editor::FDocumentId LastRequestedId;
		int OpenCount = 0;
		int FocusCount = 0;
		bool bDirty = false;
	};

	TEST_F(FEditorSubsystemTests, AssetEditorWorkspaceTracksActualOpenDeferredCloseAndUnregistration)
	{
		Testing::FScopedMountRegistryFixture Mounts;
		Testing::RegisterMountPointForTests("/AssetEditorTests/", Testing::GetTestWorkDirectory().generic_string() + "/");
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/AssetEditorTests/Package", Path));
		auto* Package = CreatePackage(Path);
		ASSERT_NE(nullptr, Package);
		TStrongObjectPtr<DPackage> KeepPackage(Package);
		auto* Asset = NewObject<DObject>(Package, "Asset");
		auto* Host = NewHost();
		auto* Service = Host->GetEditorSubsystem<DAssetEditorSubsystem>();
		{
			Editor::FWorkspaceManager Manager(Service);
			auto Workspace = std::make_shared<FAssetEditorWorkspace>();
			Editor::FWorkspaceRegistrationBatch Batch;
			Batch.Workspaces.push_back({{.WorkspaceType = Workspace->Type, .DisplayName = "Test", .RootKey = "AssetEditorTest"}, Workspace});
			Batch.AssetEditors.push_back({.AssetClassName = "Durin::DObject", .WorkspaceType = Workspace->Type});
			auto Registration = Manager.RegisterBatch(std::move(Batch));
			ASSERT_TRUE(Registration);
			EXPECT_TRUE(Service->CanOpenEditorForAsset(Asset));
			EXPECT_TRUE(Service->OpenEditorForAsset(Asset));
			ASSERT_EQ(1u, Service->GetAllOpenEditors().size());
			EXPECT_TRUE(Service->OpenEditorForAsset(Asset));
			EXPECT_EQ(1, Workspace->OpenCount);
			Workspace->bDirty = true;
			EXPECT_EQ(0, Service->CloseAllEditorsForAsset(Asset));
			ASSERT_NE(nullptr, Manager.GetPendingCloseDocument());
			EXPECT_EQ(1u, Service->GetAllOpenEditors().size());
			EXPECT_EQ(Editor::EDocumentCloseResult::Closed, Manager.ResolvePendingDocumentClose(Editor::EDocumentCloseResponse::Discard));
			EXPECT_TRUE(Service->GetAllOpenEditors().empty());
			Workspace->OpenResult = Editor::EDocumentOpenResult::Deferred;
			EXPECT_TRUE(Service->OpenEditorForAsset(Asset));
			EXPECT_TRUE(Service->GetAllOpenEditors().empty());
			ASSERT_TRUE(Manager.CompleteDeferredDocumentOpen(Workspace->LastRequestedId, true));
			EXPECT_EQ(1u, Service->GetAllOpenEditors().size());
			Registration.Reset();
			EXPECT_TRUE(Service->GetAllOpenEditors().empty());
			EXPECT_FALSE(Service->CanOpenEditorForAsset(Asset));
		}
		EXPECT_FALSE(Service->OpenEditorForAsset(Asset));
		MarkObjectHierarchyAsGarbage(Package);
	}

	TEST_F(FEditorSubsystemTests, AssetEditorSingletonKeepsItsOldAssociationUntilDeferredReplacementCommits)
	{
		Testing::FScopedMountRegistryFixture Mounts;
		Testing::RegisterMountPointForTests("/AssetEditorTests/", Testing::GetTestWorkDirectory().generic_string() + "/");
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/AssetEditorTests/Singleton", Path));
		auto* Package = CreatePackage(Path);
		ASSERT_NE(nullptr, Package);
		TStrongObjectPtr<DPackage> KeepPackage(Package);
		auto* First = NewObject<DObject>(Package, "First");
		auto* Second = NewObject<DObject>(Package, "Second");
		auto* Service = NewHost()->GetEditorSubsystem<DAssetEditorSubsystem>();
		{
			Editor::FWorkspaceManager Manager(Service);
			auto Workspace = std::make_shared<FAssetEditorWorkspace>();
			Editor::FWorkspaceRegistrationBatch Batch;
			Batch.Workspaces.push_back({{.WorkspaceType = Workspace->Type, .DisplayName = "Test", .RootKey = "SingletonTest"}, Workspace});
			Batch.AssetEditors.push_back({.AssetClassName = "Durin::DObject", .WorkspaceType = Workspace->Type,
				.DocumentPolicy = Editor::EDocumentPolicy::Singleton, .SingletonDocumentKey = "Shared", .SingletonLabel = "Shared"});
			auto Registration = Manager.RegisterBatch(std::move(Batch));
			ASSERT_TRUE(Registration);
			EXPECT_TRUE(Service->OpenEditorForAssets({First, First}));
			EXPECT_EQ(1, Workspace->OpenCount);
			auto* Instance = Service->FindEditorForAsset(First, false);
			ASSERT_NE(nullptr, Instance);
			Workspace->OpenResult = Editor::EDocumentOpenResult::Deferred;
			EXPECT_TRUE(Service->OpenEditorForAsset(Second));
			EXPECT_EQ(Instance, Service->FindEditorForAsset(First, false));
			EXPECT_EQ(nullptr, Service->FindEditorForAsset(Second, false));
			ASSERT_TRUE(Manager.CompleteDeferredDocumentOpen(Workspace->LastRequestedId, false));
			EXPECT_EQ(Instance, Service->FindEditorForAsset(First, false));
			EXPECT_TRUE(Service->OpenEditorForAsset(Second));
			ASSERT_TRUE(Manager.CompleteDeferredDocumentOpen(Workspace->LastRequestedId, true));
			EXPECT_EQ(nullptr, Service->FindEditorForAsset(First, false));
			EXPECT_EQ(Instance, Service->FindEditorForAsset(Second, false));
			Manager.RemapResourceId(Second->GetObjectPath(), First->GetObjectPath());
			EXPECT_EQ(Instance, Service->FindEditorForAsset(First, false));
			EXPECT_EQ(nullptr, Service->FindEditorForAsset(Second, false));
		}
		EXPECT_TRUE(Service->GetAllOpenEditors().empty());
		MarkObjectHierarchyAsGarbage(Package);
	}
}
