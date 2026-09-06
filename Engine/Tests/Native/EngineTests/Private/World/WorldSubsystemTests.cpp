#include "WorldTestSupport.h"
#include "DObject/StrongObjectPtr.h"
#include "Collision/CollisionDebugSubsystem.h"

namespace Durin
{
	// Native fixtures exercise the same class factory path as registered module services.
	class FSubsystemProbeA : public DWorldSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FSubsystemProbeA, DWorldSubsystem, FSubsystemProbeA::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FSubsystemProbeA)
	public:
		explicit FSubsystemProbeA(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
		auto Initialize() -> FWorldSubsystemResult override { Event("Init"); return Failure ? FWorldSubsystemResult{EWorldSubsystemError::InitializationFailed, "fixture"} : FWorldSubsystemResult{}; }
		auto Deinitialize() noexcept -> void override { Event("Deinit"); }
		auto OnWorldBeginPlay() noexcept -> void override { Event("Begin"); }
		auto OnWorldEndPlay() noexcept -> void override { Event("End"); }
		auto OnLevelAttached(DLevel&) noexcept -> void override { Event("Attach"); }
		auto OnLevelDetached(DLevel&) noexcept -> void override { Event("Detach"); }
		auto Tick(float) noexcept -> void override { Event("Tick"); }
		auto Event(std::string_view Name) -> void { if (Callback) Callback(*this, Name); }
		bool Failure = false;
		inline static std::function<void(FSubsystemProbeA&, std::string_view)> Callback;
	};
	class FSubsystemProbeB : public FSubsystemProbeA
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FSubsystemProbeB, FSubsystemProbeA, FSubsystemProbeB::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FSubsystemProbeB)
	public:
		explicit FSubsystemProbeB(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FSubsystemProbeA)
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FSubsystemProbeB)
}

namespace
{
	using namespace Durin;
	class FWorldSubsystemTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override { InitializeDObjectSystem(); }
		auto TearDown() -> void override
		{
			FSubsystemProbeA::Callback = {};
			for (auto& World : Worlds) { World->Shutdown(); MarkObjectHierarchyAsGarbage(World.Get()); }
			Worlds.clear();
			CollectGarbage();
		}
		auto MakeWorld(EWorldType Type = EWorldType::Game) -> DWorld*
		{
			auto* World = NewObject<DWorld>(nullptr, "SubsystemWorld");
			World->SetWorldType(Type);
			Worlds.emplace_back(World);
			return World;
		}
		std::vector<TStrongObjectPtr<DWorld>> Worlds;
	};
}

TEST_F(FWorldSubsystemTests, OrdersDependenciesRetainsObjectsAndFreezesRegistration)
{
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .Dependencies = {FSubsystemProbeA::StaticClass()}});
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	std::vector<DClass*> Order;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init") { Order.push_back(Object.GetClass()); EXPECT_EQ(Object.GetWorld()->GetSubsystem<FSubsystemProbeB>(), nullptr); }
	};
	auto* First = MakeWorld();
	EXPECT_EQ(First->GetSubsystem<FSubsystemProbeA>(), nullptr);
	ASSERT_TRUE(First->InitializeSubsystems());
	EXPECT_EQ(Order, (std::vector<DClass*>{FSubsystemProbeA::StaticClass(), FSubsystemProbeB::StaticClass()}));
	auto* AInstance = First->GetSubsystem<FSubsystemProbeA>();
	ASSERT_NE(AInstance, nullptr);
	EXPECT_EQ(AInstance->GetOuter(), First);
	CollectGarbage();
	EXPECT_EQ(First->GetSubsystem<FSubsystemProbeA>(), AInstance);
	auto* Second = MakeWorld();
	ASSERT_TRUE(Second->InitializeSubsystems());
	EXPECT_NE(Second->GetSubsystem<FSubsystemProbeA>(), AInstance);
	EXPECT_FALSE(First->SetWorldType(EWorldType::Editor));
	EXPECT_EQ(First->InitializeSubsystems().Error, EWorldSubsystemError::InvalidState);
	FWorldSubsystemRegistration Duplicate({.Type = FSubsystemProbeA::StaticClass()});
	EXPECT_EQ(First->GetSubsystem<FSubsystemProbeA>(), AInstance);
	EXPECT_EQ(MakeWorld()->InitializeSubsystems().Error, EWorldSubsystemError::DuplicateType);
}

TEST_F(FWorldSubsystemTests, FiltersBeforeResolvingMissingDependenciesAndRejectsCycles)
{
	{
		FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .WorldTypes = {EWorldType::Editor}});
		FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .Dependencies = {FSubsystemProbeA::StaticClass()}});
		EXPECT_EQ(MakeWorld()->InitializeSubsystems().Error, EWorldSubsystemError::MissingDependency);
		EXPECT_TRUE(MakeWorld(EWorldType::Editor)->InitializeSubsystems());
	}
	{
		FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .Dependencies = {FSubsystemProbeB::StaticClass()}});
		FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .Dependencies = {FSubsystemProbeA::StaticClass()}});
		EXPECT_EQ(MakeWorld()->InitializeSubsystems().Error, EWorldSubsystemError::DependencyCycle);
	}
}

TEST_F(FWorldSubsystemTests, CleansFailedServiceThenUnwindsDependenciesAndClosesWork)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .Dependencies = {FSubsystemProbeA::StaticClass()}});
	std::vector<DClass*> Cleanup;
	std::shared_ptr<const FWorldSubsystemWorkGate> Gate;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init" && Object.GetClass() == FSubsystemProbeB::StaticClass()) { Object.Failure = true; Gate = Object.GetWorkGate(); }
		if (Event == "Deinit") { Cleanup.push_back(Object.GetClass()); EXPECT_FALSE(Object.GetWorkGate()->IsOpen()); }
	};
	auto* World = MakeWorld();
	EXPECT_EQ(World->InitializeSubsystems().Error, EWorldSubsystemError::InitializationFailed);
	EXPECT_EQ(World->GetSubsystemState(), EWorldSubsystemState::Failed);
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_EQ(Cleanup, (std::vector<DClass*>{FSubsystemProbeB::StaticClass(), FSubsystemProbeA::StaticClass()}));
	ASSERT_NE(Gate, nullptr);
	EXPECT_FALSE(Gate->IsOpen());
	World->Shutdown(); World->Shutdown();
	EXPECT_EQ(Cleanup.size(), 2u);
}

TEST_F(FWorldSubsystemTests, SurvivesLevelReplacementAndPairsRepeatedPlay)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld();
	ASSERT_TRUE(World->InitializeSubsystems());
	auto* Instance = World->GetSubsystem<FSubsystemProbeA>();
	std::vector<std::string> Events;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		Events.emplace_back(Event);
		if (Event == "Attach" || Event == "Detach") EXPECT_NE(Object.GetWorld()->GetCurrentLevel(), nullptr);
	};
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "First")));
	ASSERT_TRUE(World->BeginPlay({})); World->EndPlay();
	ASSERT_TRUE(World->BeginPlay({})); World->EndPlay();
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Second")));
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), Instance);
	World->Shutdown();
	EXPECT_EQ(Events, (std::vector<std::string>{"Attach", "Begin", "End", "Begin", "End", "Detach", "Attach", "Detach", "Deinit"}));
}

TEST_F(FWorldSubsystemTests, DefersRetirementUntilCallbackUnwindsAndStopsLaterCallbacks)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
	std::vector<std::string> Events;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		Events.emplace_back(Event);
		if (Event == "Begin") {
			EXPECT_EQ(Object.GetClass(), FSubsystemProbeA::StaticClass());
			World->Shutdown();
			EXPECT_EQ(World->GetSubsystemState(), EWorldSubsystemState::Ready);
			EXPECT_NE(World->GetCurrentLevel(), nullptr);
		}
	};
	EXPECT_EQ(World->BeginPlay({}).Error, EWorldPlayError::PlayAborted);
	EXPECT_EQ(World->GetSubsystemState(), EWorldSubsystemState::Shutdown);
	EXPECT_EQ(std::ranges::count(Events, "Begin"), 1);
	EXPECT_EQ(std::ranges::count(Events, "End"), 1);
	EXPECT_EQ(std::ranges::count(Events, "Deinit"), 2);
}

TEST_F(FWorldSubsystemTests, AdmitsEmptyPreviewAndGameplayPauseStepWithNextFrameTickMutation)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .bTick = true, .bTickInEditorAndPreview = true});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .bTick = true, .bTickInEditorAndPreview = true});
	auto* Preview = MakeWorld(EWorldType::Preview); ASSERT_TRUE(Preview->InitializeSubsystems());
	int Ticks = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event != "Tick") return;
		++Ticks;
		if (Object.GetClass() == FSubsystemProbeA::StaticClass()) Object.GetWorld()->GetSubsystem<FSubsystemProbeB>()->SetTickEnabled(false);
	};
	Preview->Tick({}); EXPECT_EQ(Ticks, 2);
	Preview->Tick({}); EXPECT_EQ(Ticks, 3);
	auto* Game = MakeWorld(); ASSERT_TRUE(Game->InitializeSubsystems());
	Game->Tick({}); EXPECT_EQ(Ticks, 3);
	ASSERT_TRUE(Game->SetCurrentLevel(NewObject<DLevel>(Game, "Level")));
	ASSERT_TRUE(Game->BeginPlay({}));
	Game->SetPaused(true); Game->Tick({}); EXPECT_EQ(Ticks, 3);
	Game->RequestSingleStep(); Game->Tick({}); EXPECT_EQ(Ticks, 5);
	Game->Tick({}); EXPECT_EQ(Ticks, 5);
	Game->SetPaused(false); Game->Tick({}); EXPECT_EQ(Ticks, 6);
}

TEST_F(FWorldSubsystemTests, CollisionStateIsIsolatedAndClearedAcrossDetachShutdownAndDuplication)
{
	auto* Editor = MakeWorld(EWorldType::Editor);
	auto* PIE = MakeWorld(EWorldType::PlayInEditor);
	auto* Preview = MakeWorld(EWorldType::Preview);
	for (auto* World : {Editor, PIE, Preview}) ASSERT_TRUE(World->InitializeSubsystems());
	Editor->SetCollisionDebugDrawEnabled(true);
	EXPECT_FALSE(PIE->IsCollisionDebugDrawEnabled()); EXPECT_FALSE(Preview->IsCollisionDebugDrawEnabled());
	ASSERT_TRUE(Editor->SetCurrentLevel(NewObject<DLevel>(Editor, "Level")));
	FHitResult Hit; Hit.bBlockingHit = true;
	Editor->GetSubsystem<DCollisionDebugSubsystem>()->RecordHit(Hit);
	EXPECT_TRUE(Editor->CaptureCollisionDebugSnapshot().LastBlockingHit.has_value());
	ASSERT_TRUE(Editor->SetCurrentLevel(nullptr));
	EXPECT_FALSE(Editor->CaptureCollisionDebugSnapshot().LastBlockingHit.has_value());
	EXPECT_TRUE(Editor->IsCollisionDebugDrawEnabled());
	std::unordered_map<DObject*, DObject*> Copies;
	auto* Copy = DuplicateObject(Editor, nullptr, "Copy", &Copies);
	ASSERT_NE(Copy, nullptr); Worlds.emplace_back(Copy);
	EXPECT_EQ(Copy->GetSubsystem<DCollisionDebugSubsystem>(), nullptr);
	EXPECT_EQ(Copies.size(), 1u);
	PIE->Shutdown(); Preview->Shutdown();
	EXPECT_TRUE(Editor->IsCollisionDebugDrawEnabled());
	Editor->Shutdown(); EXPECT_FALSE(Editor->IsCollisionDebugDrawEnabled());
}

TEST_F(FWorldSubsystemTests, RejectsProviderRetirementUntilGarbageObjectsReleaseCode)
{
	class FProvider : public IModuleInterface {};
	FModuleTestHarness::InstallStartedModule("WorldSubsystemFixtureProvider", std::make_unique<FProvider>());
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .Provider = "WorldSubsystemFixtureProvider"});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	auto Gate = World->GetSubsystem<FSubsystemProbeA>()->GetWorkGate();
	auto& Manager = FModuleManager::Get();
	EXPECT_EQ(Manager.ShutdownModule("WorldSubsystemFixtureProvider").Status, EModuleOperationStatus::OutstandingCodeLease);
	EXPECT_TRUE(Manager.IsModuleLoaded("WorldSubsystemFixtureProvider"));
	World->Shutdown();
	EXPECT_EQ(Manager.ShutdownModule("WorldSubsystemFixtureProvider").Status, EModuleOperationStatus::OutstandingCodeLease);
	CollectGarbage();
	EXPECT_EQ(Manager.ShutdownModule("WorldSubsystemFixtureProvider").Status, EModuleOperationStatus::OutstandingCodeLease);
	Gate.reset();
	EXPECT_TRUE(Manager.ShutdownModule("WorldSubsystemFixtureProvider").Succeeded());
	EXPECT_EQ(MakeWorld()->InitializeSubsystems().Error, EWorldSubsystemError::ProviderUnavailable);
}

TEST_F(FWorldSubsystemTests, LateDetachedCompletionCannotPublishAfterWorldRetirement)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	auto Gate = World->GetSubsystem<FSubsystemProbeA>()->GetWorkGate();
	struct FDetachedPayload { std::mutex Mutex; std::condition_variable Condition; bool bReleased = false; int Value = 0; };
	auto Payload = std::make_shared<FDetachedPayload>();
	auto Worker = LaunchTask("SubsystemDetachedWork", [Payload] {
		std::unique_lock Lock(Payload->Mutex);
		Payload->Condition.wait(Lock, [&] { return Payload->bReleased; });
		Payload->Value = 42;
	});
	World->Shutdown();
	{ std::lock_guard Lock(Payload->Mutex); Payload->bReleased = true; }
	Payload->Condition.notify_all();
	EXPECT_EQ(WaitTask(Worker).WaitStatus, ETaskWaitStatus::Completed);
	int Published = 0;
	// This is the owner's GameThread completion boundary; no retired object is resolved.
	if (Gate->IsOpen()) Published = Payload->Value;
	EXPECT_EQ(Published, 0);
	EXPECT_TRUE(Gate->GetCancellationToken().IsCancellationRequested());
}

TEST_F(FWorldSubsystemTests, InitializationRetainsUnpublishedWorldDuringCollection)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FSubsystemProbeA::Callback = [](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init") { CollectGarbage(); EXPECT_FALSE(Object.IsPendingKill()); EXPECT_NE(Object.GetWorld(), nullptr); }
	};
	auto* World = NewObject<DWorld>(nullptr, "UnpublishedWorld");
	ASSERT_TRUE(World->InitializeSubsystems());
	Worlds.emplace_back(World);
	EXPECT_NE(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
}

TEST_F(FWorldSubsystemTests, TickEndPlayStopsLaterServicesAndRetainsThemForAnotherPlay)
{
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .bTick = true});
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .bTick = true});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
	ASSERT_TRUE(World->BeginPlay({}));
	int Ticks = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Tick") {
			++Ticks; EXPECT_EQ(Object.GetClass(), FSubsystemProbeA::StaticClass());
			World->EndPlay(); EXPECT_TRUE(World->HasBegunPlay());
		}
	};
	World->Tick({});
	EXPECT_EQ(Ticks, 1); EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_EQ(World->GetSubsystemState(), EWorldSubsystemState::Ready);
	ASSERT_TRUE(World->BeginPlay({}));
}

TEST_F(FWorldSubsystemTests, AttachmentTransitionStopsCallbacksAndAppliesAtNextTick)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	auto* First = NewObject<DLevel>(World, "First");
	auto* Second = NewObject<DLevel>(World, "Second");
	int AttachCount = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event != "Attach") return;
		++AttachCount;
		if (Object.GetWorld()->GetCurrentLevel() == First) {
			EXPECT_FALSE(World->SetCurrentLevel(Second));
			EXPECT_TRUE(World->RequestLevelTransition(Second));
			CollectGarbage();
		}
	};
	ASSERT_TRUE(World->SetCurrentLevel(First));
	EXPECT_EQ(AttachCount, 1);
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Second);
	EXPECT_EQ(AttachCount, 3);
}

TEST_F(FWorldSubsystemTests, ServiceSpawnedActorsBeginOnlyAfterAllServicesAndBootstrapFailureHasNoPlayCallbacks)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
	int BeginCount = 0;
	AActor* Spawned = nullptr;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event != "Begin") return;
		++BeginCount;
		if (Object.GetClass() == FSubsystemProbeA::StaticClass()) Spawned = World->SpawnActor<AActor>("ServiceActor");
		EXPECT_TRUE(Spawned && !Spawned->HasBegunPlay());
	};
	EXPECT_EQ(World->BeginPlay({.GameModeClass = AGameMode::StaticClass()}).Error, EWorldPlayError::MissingPlayerStart);
	EXPECT_EQ(BeginCount, 0);
	ASSERT_TRUE(World->BeginPlay({}));
	EXPECT_EQ(BeginCount, 2);
	ASSERT_NE(Spawned, nullptr); EXPECT_TRUE(Spawned->HasBegunPlay());
}

TEST_F(FWorldSubsystemTests, LevelSwitchRetainsBothLevelsAcrossExtensionCollections)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	auto* First = NewObject<DLevel>(World, "First");
	ASSERT_TRUE(World->SetCurrentLevel(First));
	auto* Second = NewObject<DLevel>(World, "Second");
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA&, std::string_view Event) {
		if (Event == "Attach" || Event == "Detach") {
			CollectGarbage();
			EXPECT_FALSE(First->IsPendingKill());
			EXPECT_FALSE(Second->IsPendingKill());
		}
	};
	ASSERT_TRUE(World->SetCurrentLevel(Second));
	EXPECT_EQ(World->GetCurrentLevel(), Second);
	EXPECT_TRUE(First->IsPendingKill());
}
