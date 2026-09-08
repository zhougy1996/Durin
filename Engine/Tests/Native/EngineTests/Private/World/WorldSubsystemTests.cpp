#include "WorldTestSupport.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorNotificationSubsystem.h"
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

TEST_F(FWorldSubsystemTests, TransitionOwnsTargetAcrossEndPlayAndPreservesNextRequest)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	auto* First = NewObject<DLevel>(World, "First");
	ASSERT_TRUE(World->SetCurrentLevel(First));
	ASSERT_TRUE(World->BeginPlay({}));
	auto* Second = NewObject<DLevel>(World, "Second");
	auto* Third = NewObject<DLevel>(World, "Third");
	bool bRequested = false;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA&, std::string_view Event) {
		if (Event != "End" || bRequested) return;
		bRequested = true;
		EXPECT_TRUE(IsGarbageCollectionDeferred());
		CollectGarbage();
		EXPECT_TRUE(IsGarbageCollectionRequested());
		EXPECT_FALSE(Second->IsPendingKill());
		EXPECT_FALSE(World->SetCurrentLevel(Third));
		EXPECT_TRUE(World->RequestLevelTransition(Third));
	};
	ASSERT_TRUE(World->RequestLevelTransition(Second));
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Second);
	EXPECT_FALSE(Second->IsPendingKill());
	CollectGarbage();
	EXPECT_FALSE(Third->IsPendingKill());
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Third);
}

TEST_F(FWorldSubsystemTests, TickDefersCollectionAndRejectsRecursiveWorldOperations)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .bTick = true});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
	ASSERT_TRUE(World->BeginPlay({}));
	TObjectPtr<DObject> Garbage = NewObject<DObject>(nullptr, "DeferredGarbage");
	int Ticks = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA&, std::string_view Event) {
		if (Event != "Tick") return;
		++Ticks;
		World->Tick({});
		World->EndPlay();
		EXPECT_FALSE(World->BeginPlay({}));
		EXPECT_TRUE(World->HasBegunPlay());
		MarkAsGarbage(Garbage.Get());
		CollectGarbage();
		EXPECT_NE(Garbage.Get(), nullptr);
	};
	World->Tick({});
	EXPECT_EQ(Ticks, 1);
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_FALSE(IsGarbageCollectionDeferred());
	CollectGarbage();
	EXPECT_EQ(Garbage.Get(), nullptr);
	ASSERT_TRUE(World->BeginPlay({}));
	FSubsystemProbeA::Callback = {};
	World->Tick({}); // Previous frame must have ended before the next StartFrame.
}

TEST_F(FWorldSubsystemTests, EndPlayCanQueueReturnToOriginalLevelDuringTransition)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld(); ASSERT_TRUE(World->InitializeSubsystems());
	auto* First = NewObject<DLevel>(World, "First");
	ASSERT_TRUE(World->SetCurrentLevel(First));
	ASSERT_TRUE(World->BeginPlay({}));
	auto* Second = NewObject<DLevel>(World, "Second");
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA&, std::string_view Event) {
		if (Event == "End") EXPECT_TRUE(World->RequestLevelTransition(First));
	};
	ASSERT_TRUE(World->RequestLevelTransition(Second, false));
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Second);
	CollectGarbage();
	EXPECT_FALSE(First->IsPendingKill());
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), First);
	FSubsystemProbeA::Callback = {};
}

TEST_F(FWorldSubsystemTests, RejectsWrongScopeAndReportsMissingDependencyIdentity)
{
	{
		FWorldSubsystemRegistration Invalid({.Type = DObject::StaticClass()});
		EXPECT_EQ(MakeWorld()->InitializeSubsystems().Error, EWorldSubsystemError::InvalidDescriptor);
	}
	{
		FWorldSubsystemRegistration Missing({.Type = FSubsystemProbeA::StaticClass(), .Dependencies = {FSubsystemProbeB::StaticClass()}});
		const auto Result = MakeWorld()->InitializeSubsystems();
		EXPECT_EQ(Result.Error, EWorldSubsystemError::MissingDependency);
		EXPECT_NE(Result.Message.find("FSubsystemProbeA"), std::string::npos);
		EXPECT_NE(Result.Message.find("FSubsystemProbeB"), std::string::npos);
	}
}

TEST_F(FWorldSubsystemTests, ExceptionRollsBackFailedObjectBeforeDependencies)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .Dependencies = {FSubsystemProbeA::StaticClass()}});
	std::vector<DClass*> Cleanup;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init" && Object.GetClass() == FSubsystemProbeB::StaticClass()) throw std::runtime_error("fixture");
		if (Event == "Deinit") Cleanup.push_back(Object.GetClass());
	};
	auto* World = MakeWorld();
	EXPECT_EQ(World->InitializeSubsystems().Error, EWorldSubsystemError::InitializationFailed);
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_EQ(Cleanup, (std::vector<DClass*>{FSubsystemProbeB::StaticClass(), FSubsystemProbeA::StaticClass()}));
}

TEST_F(FWorldSubsystemTests, ShutdownDuringInitializeClosesGateBeforeDeferredCleanup)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	bool bInsideInitialize = false;
	int Cleanup = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init") {
			bInsideInitialize = true;
			Object.GetWorld()->Shutdown();
			EXPECT_FALSE(Object.GetWorkGate()->IsOpen());
			EXPECT_EQ(Cleanup, 0);
			bInsideInitialize = false;
		}
		if (Event == "Deinit") { EXPECT_FALSE(bInsideInitialize); ++Cleanup; }
	};
	auto* World = MakeWorld();
	EXPECT_EQ(World->InitializeSubsystems().Error, EWorldSubsystemError::Aborted);
	EXPECT_EQ(Cleanup, 1);
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_EQ(World->InitializeSubsystems().Error, EWorldSubsystemError::InvalidState);
}

namespace Durin
{
	// CPU host fixture invokes the production collection boot boundary without renderer startup.
	class FSubsystemEditorHost : public DEditorEngine
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FSubsystemEditorHost, DEditorEngine, FSubsystemEditorHost::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FSubsystemEditorHost)
	public:
		explicit FSubsystemEditorHost(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
		auto SetEditorWorld(DWorld* World) -> void { EditorWorld = World; SetWorld(World); }
		auto StartPlay(DLevel* Level) -> bool { return StartPlaySessionInternal({.SourceLevel = Level}, std::optional<DClass*>{nullptr}, nullptr); }
		auto StartEngine() -> FSubsystemResult { return InitializeEngineSubsystems(); }
		auto StartEditor() -> FSubsystemResult { return InitializeEditorSubsystems(); }
		auto Dispatch(const std::function<void()>& Callback) -> void { FHostOperationScope Operation(*this); Callback(); }
	};
	class FEngineSubsystemProbe : public DEngineSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FEngineSubsystemProbe, DEngineSubsystem, FEngineSubsystemProbe::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FEngineSubsystemProbe)
	public:
		explicit FEngineSubsystemProbe(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) { if (ConstructorCallback) ConstructorCallback(*this); }
		auto Initialize() -> FSubsystemResult override { if (Callback) Callback(*this, true); return {}; }
		auto Deinitialize() noexcept -> void override { if (Callback) Callback(*this, false); }
		inline static std::function<void(FEngineSubsystemProbe&, bool)> Callback;
		inline static std::function<void(FEngineSubsystemProbe&)> ConstructorCallback;
	};
	class FEditorSubsystemProbe : public DEditorSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FEditorSubsystemProbe, DEditorSubsystem, FEditorSubsystemProbe::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FEditorSubsystemProbe)
	public:
		explicit FEditorSubsystemProbe(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) {}
		auto Initialize() -> FSubsystemResult override { if (Callback) Callback(*this, true); return {}; }
		auto Deinitialize() noexcept -> void override { if (Callback) Callback(*this, false); }
		inline static std::function<void(FEditorSubsystemProbe&, bool)> Callback;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FSubsystemEditorHost)
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FEngineSubsystemProbe)
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FEditorSubsystemProbe)
}

TEST_F(FWorldSubsystemTests, HostsKeepIndependentCollectionsAcrossWorldLifetimesAndGC)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FEditorSubsystemRegistration EditorToken({.Type = FEditorSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> First(NewObject<FSubsystemEditorHost>(nullptr, {}));
	TStrongObjectPtr<FSubsystemEditorHost> Second(NewObject<FSubsystemEditorHost>(nullptr, {}));
	for (auto* Host : {First.Get(), Second.Get()}) { ASSERT_TRUE(Host->StartEngine()); ASSERT_TRUE(Host->StartEditor()); }
	auto* EngineService = First->GetSubsystem<FEngineSubsystemProbe>();
	auto* EditorService = First->GetEditorSubsystem<FEditorSubsystemProbe>();
	auto* Notifications = &First->GetNotificationManager();
	ASSERT_NE(EngineService, nullptr); ASSERT_NE(EditorService, nullptr);
	EXPECT_EQ(EngineService->GetEngine(), First.Get());
	EXPECT_EQ(EditorService->GetEditor(), First.Get());
	EXPECT_NE(Second->GetSubsystem<FEngineSubsystemProbe>(), EngineService);
	auto* EditorWorld = NewObject<DWorld>(First.Get(), {});
	Worlds.emplace_back(EditorWorld);
	EditorWorld->SetWorldType(EWorldType::Editor);
	ASSERT_TRUE(EditorWorld->InitializeSubsystems());
	ASSERT_TRUE(EditorWorld->SetCurrentLevel(NewObject<DLevel>(EditorWorld, "EditorLevel")));
	First->SetEditorWorld(EditorWorld);
	for (int Session = 0; Session < 2; ++Session) {
		ASSERT_TRUE(First->StartPlay(EditorWorld->GetCurrentLevel()));
		EXPECT_NE(First->GetPlayWorld(), nullptr);
		First->StopPlaySession();
		EXPECT_EQ(First->GetWorld(), EditorWorld);
		EXPECT_EQ(First->GetSubsystem<FEngineSubsystemProbe>(), EngineService);
		EXPECT_EQ(First->GetEditorSubsystem<FEditorSubsystemProbe>(), EditorService);
		EXPECT_EQ(&First->GetNotificationManager(), Notifications);
	}
	CollectGarbage();
	EXPECT_EQ(First->GetSubsystem<FEngineSubsystemProbe>(), EngineService);
	EXPECT_EQ(First->GetEditorSubsystem<FEditorSubsystemProbe>(), EditorService);
	First->PrepareForShutdown(); First->PrepareForShutdown();
	EXPECT_EQ(First->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	EXPECT_NE(Second->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	Second->PrepareForShutdown();
}

TEST_F(FWorldSubsystemTests, HostShutdownDuringInitializationAndDispatchIsDeferred)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	int Cleanups = 0;
	FEngineSubsystemProbe::Callback = [&](FEngineSubsystemProbe& Service, bool bInit) {
		if (bInit) {
			Service.GetEngine()->PrepareForShutdown();
			EXPECT_FALSE(Service.GetWorkGate()->IsOpen());
			EXPECT_EQ(Cleanups, 0);
		} else ++Cleanups;
	};
	EXPECT_EQ(Host->StartEngine().Error, ESubsystemError::Aborted);
	EXPECT_EQ(Cleanups, 1);
	FEngineSubsystemProbe::Callback = {};
	TStrongObjectPtr<FSubsystemEditorHost> Ready(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_TRUE(Ready->StartEngine());
	auto Gate = Ready->GetSubsystem<FEngineSubsystemProbe>()->GetWorkGate();
	Ready->Dispatch([&] {
		Ready->PrepareForShutdown();
		EXPECT_FALSE(Gate->IsOpen());
		EXPECT_NE(Ready->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
		CollectGarbage();
	});
	EXPECT_EQ(Ready->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
}

TEST_F(FWorldSubsystemTests, WorldAndEditorCleanupRetainEngineDependencies)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FEditorSubsystemRegistration EditorToken({.Type = FEditorSubsystemProbe::StaticClass()});
	FWorldSubsystemRegistration WorldToken({.Type = FSubsystemProbeA::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_TRUE(Host->StartEngine()); ASSERT_TRUE(Host->StartEditor());
	auto* World = NewObject<DWorld>(Host.Get(), {}); Worlds.emplace_back(World);
	ASSERT_TRUE(World->InitializeSubsystems()); Host->SetWorld(World);
	int Checks = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA&, std::string_view Event) {
		if (Event == "Deinit") { EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr); ++Checks; }
	};
	FEditorSubsystemProbe::Callback = [&](FEditorSubsystemProbe&, bool bInit) {
		if (!bInit) { EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr); ++Checks; }
	};
	Host->PrepareForShutdown();
	EXPECT_EQ(Checks, 2);
	FEditorSubsystemProbe::Callback = {};
}

TEST_F(FWorldSubsystemTests, EngineAndEditorRegistrationsRejectUnrelatedTypes)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEditorSubsystemProbe::StaticClass()});
	FEditorSubsystemRegistration EditorToken({.Type = FSubsystemProbeA::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	EXPECT_EQ(Host->StartEngine().Error, ESubsystemError::InvalidDescriptor);
	EXPECT_EQ(Host->StartEditor().Error, ESubsystemError::InvalidDescriptor);
	Host->PrepareForShutdown();
}

TEST_F(FWorldSubsystemTests, EditorInitializationCanRequestShutdownOrThrow)
{
	FEditorSubsystemRegistration Token({.Type = FEditorSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_TRUE(Host->StartEngine());
	int Cleanups = 0;
	FEditorSubsystemProbe::Callback = [&](FEditorSubsystemProbe& Service, bool bInit) {
		if (bInit) {
			Service.GetEditor()->PrepareForShutdown();
			EXPECT_FALSE(Service.GetWorkGate()->IsOpen());
			EXPECT_EQ(Cleanups, 0);
		} else ++Cleanups;
	};
	EXPECT_EQ(Host->StartEditor().Error, ESubsystemError::Aborted);
	EXPECT_EQ(Cleanups, 1);
	FEditorSubsystemProbe::Callback = {};
	TStrongObjectPtr<FSubsystemEditorHost> Failed(NewObject<FSubsystemEditorHost>(nullptr, {}));
	FEditorSubsystemProbe::Callback = [](FEditorSubsystemProbe&, bool bInit) { if (bInit) throw std::runtime_error("fixture"); };
	EXPECT_EQ(Failed->StartEditor().Error, ESubsystemError::InitializationFailed);
	EXPECT_EQ(Failed->GetEditorSubsystem<FEditorSubsystemProbe>(), nullptr);
	FEditorSubsystemProbe::Callback = {};
	Failed->PrepareForShutdown();
}

TEST_F(FWorldSubsystemTests, NotificationServiceOwnsFacadeAndRejectsRetainedActionsAfterShutdown)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_TRUE(Host->StartEngine()); ASSERT_TRUE(Host->StartEditor());
	auto* Service = Host->GetEditorSubsystem<DEditorNotificationSubsystem>();
	ASSERT_NE(Service, nullptr);
	EXPECT_EQ(&Host->GetNotificationManager(), &Service->GetManager());
	int Invocations = 0;
	Host->GetNotificationManager().Post({.Message = "Ready", .Action = Editor::FNotificationAction{
		.Label = "Open", .Invoke = [&] { ++Invocations; }}});
	Host->UpdateNotifications(0.0f);
	ASSERT_EQ(Service->GetManager().GetNotifications().size(), 1u);
	auto Action = Service->GetManager().GetNotifications().front().Action;
	auto Gate = Service->GetWorkGate();
	Host->PrepareForShutdown();
	EXPECT_FALSE(Gate->IsOpen());
	EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), nullptr);
	CollectGarbage();
	Action->Invoke();
	EXPECT_FALSE(Action->IsEnabled());
	EXPECT_EQ(Invocations, 0);
}

TEST_F(FWorldSubsystemTests, EngineRetirementImmediatelyStopsWorldCallbackAdmission)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .bTick = true, .bTickInEditorAndPreview = true});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .bTick = true, .bTickInEditorAndPreview = true});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_TRUE(Host->StartEngine());
	auto* World = MakeWorld(EWorldType::Preview);
	ASSERT_TRUE(World->InitializeSubsystems());
	Host->SetWorld(World);
	int Ticks = 0;
	bool bInsideCallback = false;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Service, std::string_view Event) {
		if (Event == "Tick") {
			++Ticks;
			bInsideCallback = true;
			Host->PrepareForShutdown();
			EXPECT_FALSE(Service.GetWorkGate()->IsOpen());
			EXPECT_FALSE(World->GetSubsystem<FSubsystemProbeB>()->GetWorkGate()->IsOpen());
			EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
			bInsideCallback = false;
		}
		if (Event == "Deinit") {
			EXPECT_FALSE(bInsideCallback);
			EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
		}
	};
	Host->Tick(0.0f, false);
	EXPECT_EQ(Ticks, 1);
	EXPECT_EQ(World->GetSubsystemState(), ESubsystemState::Shutdown);
	EXPECT_EQ(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
}

TEST_F(FWorldSubsystemTests, ConstructorRetirementDoesNotEnterInitializeWithAnOpenGate)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	int Initializations = 0;
	int Cleanups = 0;
	FEngineSubsystemProbe::ConstructorCallback = [&](FEngineSubsystemProbe&) { Host->PrepareForShutdown(); };
	FEngineSubsystemProbe::Callback = [&](FEngineSubsystemProbe& Service, bool bInit) {
		if (bInit) ++Initializations;
		else { ++Cleanups; EXPECT_FALSE(Service.GetWorkGate()->IsOpen()); }
	};
	EXPECT_EQ(Host->StartEngine().Error, ESubsystemError::Aborted);
	EXPECT_EQ(Initializations, 0);
	EXPECT_EQ(Cleanups, 1);
	FEngineSubsystemProbe::ConstructorCallback = {};
	FEngineSubsystemProbe::Callback = {};
}

TEST_F(FWorldSubsystemTests, NotificationShutdownCallbackRetiresAtTheNextHostBoundary)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_TRUE(Host->StartEngine()); ASSERT_TRUE(Host->StartEditor());
	auto* Service = Host->GetEditorSubsystem<DEditorNotificationSubsystem>();
	ASSERT_NE(Service, nullptr);
	const auto Id = Host->GetNotificationManager().Post({.Message = "Stop", .Action = Editor::FNotificationAction{
		.Label = "Stop", .Invoke = [&] {
			Host->PrepareForShutdown();
			EXPECT_FALSE(Service->GetWorkGate()->IsOpen());
			EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), Service);
			EXPECT_EQ(Service->GetManager().GetNotifications().size(), 1u);
			CollectGarbage();
			EXPECT_FALSE(Service->IsPendingKill());
		}}});
	Host->UpdateNotifications(0.0f);
	EXPECT_TRUE(Host->GetNotificationManager().InvokeAction(Id));
	EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), Service);
	Host->Tick(0.0f, false);
	EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), nullptr);
}

TEST_F(FWorldSubsystemTests, HostGarbageFallbackRetiresBothCollections)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	std::shared_ptr<const FSubsystemWorkGate> EngineGate;
	std::shared_ptr<const FSubsystemWorkGate> EditorGate;
	{
		TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
		ASSERT_TRUE(Host->StartEngine()); ASSERT_TRUE(Host->StartEditor());
		EngineGate = Host->GetSubsystem<FEngineSubsystemProbe>()->GetWorkGate();
		EditorGate = Host->GetEditorSubsystem<DEditorNotificationSubsystem>()->GetWorkGate();
	}
	CollectGarbage();
	EXPECT_FALSE(EngineGate->IsOpen());
	EXPECT_FALSE(EditorGate->IsOpen());
}

TEST_F(FWorldSubsystemTests, WrongScopeDependencyIsRejectedBeforeConstruction)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass(), .Dependencies = {FEditorSubsystemProbe::StaticClass()}});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	int Constructions = 0;
	FEngineSubsystemProbe::ConstructorCallback = [&](FEngineSubsystemProbe&) { ++Constructions; };
	const auto Result = Host->StartEngine();
	FEngineSubsystemProbe::ConstructorCallback = {};
	EXPECT_EQ(Result.Error, ESubsystemError::InvalidDescriptor);
	EXPECT_NE(Result.Message.find("FEngineSubsystemProbe"), std::string::npos);
	EXPECT_NE(Result.Message.find("FEditorSubsystemProbe"), std::string::npos);
	EXPECT_EQ(Constructions, 0);
}

namespace
{
	template<typename Host, typename Service>
	concept CHasSubsystemLookup = requires(const Host& Owner) { Owner.template GetSubsystem<Service>(); };
	template<typename Service>
	concept CHasEditorSubsystemLookup = requires(const DEditorEngine& Owner) { Owner.template GetEditorSubsystem<Service>(); };
	static_assert(CHasSubsystemLookup<DEngine, FEngineSubsystemProbe>);
	static_assert(CHasSubsystemLookup<DEditorEngine, FEngineSubsystemProbe>);
	static_assert(!CHasSubsystemLookup<DEngine, FEditorSubsystemProbe>);
	static_assert(!CHasSubsystemLookup<DWorld, FEngineSubsystemProbe>);
	static_assert(CHasEditorSubsystemLookup<FEditorSubsystemProbe>);
	static_assert(!CHasEditorSubsystemLookup<FEngineSubsystemProbe>);
}

TEST_F(FWorldSubsystemTests, UnpublishedWorldClosesAdmissionWhenItsEngineRetires)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_TRUE(Host->StartEngine());
	auto* World = NewObject<DWorld>(Host.Get(), "UnpublishedWorld");
	Worlds.emplace_back(World);
	int Initializations = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Service, std::string_view Event) {
		if (Event != "Init") return;
		++Initializations;
		EXPECT_EQ(Host->GetWorld(), nullptr);
		Host->PrepareForShutdown();
		EXPECT_FALSE(Service.GetWorkGate()->IsOpen());
		EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	};
	EXPECT_EQ(World->InitializeSubsystems().Error, ESubsystemError::Aborted);
	EXPECT_EQ(Initializations, 1);
	EXPECT_EQ(Host->GetWorld(), nullptr);
	EXPECT_EQ(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
}
