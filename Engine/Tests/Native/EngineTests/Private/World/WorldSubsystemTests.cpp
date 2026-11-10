#include "Threading/Task.h"
#include "Threading/TaskRuntime.h"
#include "WorldTestSupport.h"
#include "Editor/EditorEngineTestSupport.h"
#include "Actors/EditorActorSubsystem.h"
#include "Notifications/EditorNotificationSubsystem.h"
#include "DObject/StrongObjectPtr.h"
#include "Collision/CollisionDebugSubsystem.h"
#include "Components/SceneComponent.h"

namespace Durin
{
	auto CreateSubsystemProbeDefaults() -> bool;
	// Native fixtures exercise the same class factory path as registered module services.
	class FSubsystemProbeA : public DWorldSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FSubsystemProbeA, DWorldSubsystem, FSubsystemProbeA::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FSubsystemProbeA)
	public:
		explicit FSubsystemProbeA(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) { if (!IsClassDefaultObject()) ++InstanceConstructions; }
		inline static int InstanceConstructions = 0;
		inline static std::map<DClass*, std::vector<DClass*>> Dependencies;
		auto Initialize(FSubsystemCollection& Collection) -> void override
		{
			for (auto* Type : Dependencies[GetClass()])
				if (!Collection.InitializeDependency(Type))
				{
					if (GetWorld()->IsShutdownRequested()) return;
					requiref(false, "Required subsystem dependency is unavailable.");
				}
			Event("Init");
			if (Failure) throw std::runtime_error("fixture");
		}
		auto Deinitialize() noexcept -> void override { Cancellation->Cancel(); Event("Deinit"); }
		std::shared_ptr<Tasks::FCancellationToken> Cancellation = std::make_shared<Tasks::FCancellationToken>();
		auto OnWorldBeginPlay() noexcept -> void override { Event("Begin"); }
		auto OnWorldEndPlay() noexcept -> void override { Event("End"); }
		auto OnLevelAttached(DLevel&) noexcept -> void override { Event("Attach"); }
		auto OnLevelDetached(DLevel&) noexcept -> void override { Event("Detach"); }
		auto Tick(float) noexcept -> void override { Event("Tick"); }
		auto Event(std::string_view Name) -> void { if (Callback) Callback(*this, Name); }
		auto DoesSupportWorldType(EWorldType Type) const -> bool override
		{ return Support ? Support(*this, Type) : Super::DoesSupportWorldType(Type); }
		auto ShouldCreateSubsystem(DObject* Outer) const -> bool override
		{ return Super::ShouldCreateSubsystem(Outer) && (!Create || Create(*this, Outer)); }
		inline static std::function<bool(const FSubsystemProbeA&, EWorldType)> Support;
		inline static std::function<bool(const FSubsystemProbeA&, DObject*)> Create;
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
	// Intrinsic fixture deliberately has no CDO and must be rejected as a service.
	class FNoDefaultSubsystem : public FSubsystemProbeA
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FNoDefaultSubsystem, FSubsystemProbeA, FNoDefaultSubsystem::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FNoDefaultSubsystem)
	public:
		explicit FNoDefaultSubsystem(const FObjectInitializer& Initializer) : Super(Initializer) {}
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FNoDefaultSubsystem)
	auto FSubsystemProbeA::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FSubsystemProbeA", Class, nullptr, sizeof(FSubsystemProbeA), alignof(FSubsystemProbeA),
				EClassFlags::None, InternalConstructor<FSubsystemProbeA>, &DWorldSubsystem::StaticClass);
		return Class;
	}
	auto FSubsystemProbeB::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FSubsystemProbeB", Class, nullptr, sizeof(FSubsystemProbeB), alignof(FSubsystemProbeB),
				EClassFlags::None, InternalConstructor<FSubsystemProbeB>, &FSubsystemProbeA::StaticClass);
		return Class;
	}
}

namespace
{
	using namespace Durin;
	class FWorldSubsystemTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			ASSERT_TRUE(CreateSubsystemProbeDefaults());
			FSubsystemProbeA::InstanceConstructions = 0;
			FSubsystemProbeA::Support = [](const auto&, EWorldType) { return true; };
		}
		auto TearDown() -> void override
		{
			FSubsystemProbeA::Callback = {};
			FSubsystemProbeA::Dependencies.clear();
			FSubsystemProbeA::Support = {};
			FSubsystemProbeA::Create = {};
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

TEST_F(FWorldSubsystemTests, DefaultWorldEligibilityExcludesPreviewBeforeConstruction)
{
	FSubsystemProbeA::Support = {};
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	const auto* Default = Cast<DWorldSubsystem>(FSubsystemProbeA::StaticClass()->GetDefaultObject());
	ASSERT_NE(Default, nullptr);
	EXPECT_TRUE(Default->IsClassDefaultObject());
	EXPECT_EQ(Default->GetWorld(), nullptr);

	for (auto Type : {EWorldType::Game, EWorldType::Editor, EWorldType::PIE, EWorldType::EditorPreview})
	{
		auto* World = MakeWorld(Type);
		ASSERT_NO_THROW(World->InitializeSubsystems());
		EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>() != nullptr, Type != EWorldType::EditorPreview);
	}
	EXPECT_EQ(FSubsystemProbeA::InstanceConstructions, 3);
}

TEST_F(FWorldSubsystemTests, ValidatesTickPolicyOnlyAfterCdoEligibilityAndBeforeConstruction)
{
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .TickGroup = ETickingGroup::Count});
	int Decisions = 0;
	FSubsystemProbeA::Support = [&](const auto& Default, EWorldType Type) {
		EXPECT_TRUE(Default.IsClassDefaultObject());
		++Decisions;
		return Type != EWorldType::EditorPreview;
	};
	auto* Preview = MakeWorld(EWorldType::EditorPreview);
	ASSERT_NO_THROW(Preview->InitializeSubsystems());
	EXPECT_EQ(Decisions, 2);
	EXPECT_EQ(Preview->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_EQ(Preview->GetSubsystem<FSubsystemProbeB>(), nullptr);
	auto* Game = MakeWorld();
	EXPECT_DEATH(Game->InitializeSubsystems(), "Invalid World Tick group");
}

TEST_F(FWorldSubsystemTests, ExcludedRequiredDependencyAssertsBeforeTickValidation)
{
	FSubsystemProbeA::Dependencies[FSubsystemProbeB::StaticClass()] = {FSubsystemProbeA::StaticClass()};
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .TickGroup = ETickingGroup::Count});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	FSubsystemProbeA::Support = [](const auto& Default, EWorldType) {
		return Default.GetClass() != FSubsystemProbeA::StaticClass();
	};
	auto* World = MakeWorld();
	EXPECT_DEATH(World->InitializeSubsystems(), "Required subsystem dependency");
}

TEST_F(FWorldSubsystemTests, EligibilityUsesCdoAndHostBeforeAnyInstanceConstruction)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld();
	int Decisions = 0;
	FSubsystemProbeA::Create = [&](const auto& Default, DObject* Outer) {
		EXPECT_EQ(&Default, Default.GetClass()->GetDefaultObject());
		EXPECT_EQ(Outer, World);
		EXPECT_EQ(FSubsystemProbeA::InstanceConstructions, 0);
		++Decisions;
		return Default.GetClass() == FSubsystemProbeB::StaticClass();
	};
	ASSERT_NO_THROW(World->InitializeSubsystems());
	EXPECT_EQ(Decisions, 2);
	EXPECT_EQ(FSubsystemProbeA::InstanceConstructions, 1);
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_NE(World->GetSubsystem<FSubsystemProbeB>(), nullptr);
}

TEST_F(FWorldSubsystemTests, MissingCdoIsInvalidAndNeverFallsBackToConstructingAnInstance)
{
	FWorldSubsystemRegistration Token({.Type = FNoDefaultSubsystem::StaticClass()});
	auto* World = MakeWorld();
	EXPECT_DEATH(World->InitializeSubsystems(), "class default object");
	EXPECT_EQ(FSubsystemProbeA::InstanceConstructions, 0);
}

TEST_F(FWorldSubsystemTests, HostRetirementDuringEligibilityStopsBeforeConstructingServices)
{
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld();
	FSubsystemProbeA::Create = [&](const auto&, DObject*) { World->Shutdown(); return false; };
	EXPECT_NO_THROW(World->InitializeSubsystems());
	EXPECT_EQ(FSubsystemProbeA::InstanceConstructions, 0);
}

TEST_F(FWorldSubsystemTests, EligibilityExceptionFailsWithoutConstructingServices)
{
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	FSubsystemProbeA::Create = [](const auto&, DObject*) -> bool { throw std::runtime_error("eligibility"); };
	auto* World = MakeWorld();
	EXPECT_THROW(World->InitializeSubsystems(), std::exception);
	EXPECT_EQ(World->GetSubsystemState(), ESubsystemState::Shutdown);
	EXPECT_EQ(FSubsystemProbeA::InstanceConstructions, 0);
}

TEST_F(FWorldSubsystemTests, FailedEngineWorldInitializationDoesNotPublishContext)
{
	TStrongObjectPtr<DEngine> Engine(NewObject<DEngine>(nullptr, "FailedWorldOwner"));
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = NewObject<DWorld>(Engine.Get(), "FailedContextWorld");
	Worlds.emplace_back(World);
	FSubsystemProbeA::Callback = [](FSubsystemProbeA& Service, std::string_view Event) {
		if (Event == "Init") Service.Failure = true;
	};
	EXPECT_THROW(World->InitializeSubsystems(), std::exception);
	EXPECT_TRUE(Engine->GetWorldContexts().empty());
	World->Shutdown();
	EXPECT_TRUE(Engine->GetWorldContexts().empty());
	Engine->PrepareForShutdown();
	MarkAsGarbage(Engine.Get());
}

TEST_F(FWorldSubsystemTests, OrdersDependenciesRetainsObjectsAndFreezesRegistration)
{
	FSubsystemProbeA::Dependencies[FSubsystemProbeB::StaticClass()] = {FSubsystemProbeA::StaticClass()};
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	std::vector<DClass*> Order;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init") { Order.push_back(Object.GetClass()); EXPECT_EQ(Object.GetWorld()->GetSubsystem<FSubsystemProbeB>(), nullptr); }
	};
	auto* First = MakeWorld();
	EXPECT_EQ(First->GetSubsystem<FSubsystemProbeA>(), nullptr);
	ASSERT_NO_THROW(First->InitializeSubsystems());
	EXPECT_EQ(Order, (std::vector<DClass*>{FSubsystemProbeA::StaticClass(), FSubsystemProbeB::StaticClass()}));
	auto* AInstance = First->GetSubsystem<FSubsystemProbeA>();
	ASSERT_NE(AInstance, nullptr);
	EXPECT_EQ(AInstance->GetOuter(), First);
	CollectGarbage();
	EXPECT_EQ(First->GetSubsystem<FSubsystemProbeA>(), AInstance);
	auto* Second = MakeWorld();
	ASSERT_NO_THROW(Second->InitializeSubsystems());
	EXPECT_NE(Second->GetSubsystem<FSubsystemProbeA>(), AInstance);
	EXPECT_FALSE(First->SetWorldType(EWorldType::Editor));
	EXPECT_DEATH(First->InitializeSubsystems(), "one-shot");
	FWorldSubsystemRegistration Duplicate({.Type = FSubsystemProbeA::StaticClass()});
	EXPECT_EQ(First->GetSubsystem<FSubsystemProbeA>(), AInstance);
	EXPECT_NO_THROW(MakeWorld()->InitializeSubsystems());
}

TEST_F(FWorldSubsystemTests, FiltersBeforeResolvingMissingDependenciesAndRejectsCycles)
{
	FSubsystemProbeA::Dependencies[FSubsystemProbeB::StaticClass()] = {FSubsystemProbeA::StaticClass()};
	{
		FSubsystemProbeA::Support = [](const auto& Default, EWorldType Type) {
			return Default.GetClass() != FSubsystemProbeA::StaticClass() || Type == EWorldType::Editor;
		};
		FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
		FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
		EXPECT_DEATH(MakeWorld()->InitializeSubsystems(), "Required subsystem dependency");
		EXPECT_NO_THROW(MakeWorld(EWorldType::Editor)->InitializeSubsystems());
	}
	{
		FSubsystemProbeA::Support = [](const auto&, EWorldType) { return true; };
		FSubsystemProbeA::Dependencies[FSubsystemProbeA::StaticClass()] = {FSubsystemProbeB::StaticClass()};
		FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
		FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
		EXPECT_DEATH(MakeWorld()->InitializeSubsystems(), "Recursive subsystem dependency");
	}
}

TEST_F(FWorldSubsystemTests, CleansFailedServiceAndDependenciesOnceInReverseOrder)
{
	FSubsystemProbeA::Dependencies[FSubsystemProbeB::StaticClass()] = {FSubsystemProbeA::StaticClass()};
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	std::vector<DClass*> Cleanup;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init" && Object.GetClass() == FSubsystemProbeB::StaticClass()) { Object.Failure = true; }
		if (Event == "Deinit") { Cleanup.push_back(Object.GetClass()); }
	};
	auto* World = MakeWorld();
	EXPECT_THROW(World->InitializeSubsystems(), std::exception);
	EXPECT_EQ(World->GetSubsystemState(), EWorldSubsystemState::Shutdown);
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_EQ(Cleanup, (std::vector<DClass*>{FSubsystemProbeB::StaticClass(), FSubsystemProbeA::StaticClass()}));


	World->Shutdown(); World->Shutdown();
	EXPECT_EQ(Cleanup.size(), 2u);
}

TEST_F(FWorldSubsystemTests, FailedInitializationReleasesRetainedServiceWithoutForcingGarbage)
{
	FWorldSubsystemRegistration Registration({.Type = FSubsystemProbeA::StaticClass()});
	TStrongObjectPtr<FSubsystemProbeA> Retained;
	int Cleanups = 0;
	FSubsystemProbeA::Callback = [&](auto& Object, std::string_view Event) {
		if (Event == "Init") { Retained = &Object; Object.Failure = true; }
		if (Event == "Deinit") ++Cleanups;
	};
	auto* World = MakeWorld();
	EXPECT_THROW(World->InitializeSubsystems(), std::exception);
	ASSERT_NE(nullptr, Retained.Get());
	EXPECT_FALSE(Retained->IsGarbage());
	EXPECT_EQ(1, Cleanups);
	CollectGarbage(); EXPECT_TRUE(IsValid(Retained.Get()));
	TWeakObjectPtr<FSubsystemProbeA> Weak(Retained.Get());
	Retained.Reset(); CollectGarbage();
	EXPECT_FALSE(Weak.IsValid());
	EXPECT_EQ(EWorldSubsystemState::Shutdown, World->GetSubsystemState());
	EXPECT_EQ(1, Cleanups);
	FSubsystemProbeA::Callback = {};
}

TEST_F(FWorldSubsystemTests, SurvivesLevelReplacementAndPairsRepeatedPlay)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld();
	ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* Preview = MakeWorld(EWorldType::EditorPreview); ASSERT_NO_THROW(Preview->InitializeSubsystems());
	int Ticks = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event != "Tick") return;
		++Ticks;
		if (Object.GetClass() == FSubsystemProbeA::StaticClass()) Object.GetWorld()->GetSubsystem<FSubsystemProbeB>()->SetTickEnabled(false);
	};
	Preview->Tick({}); EXPECT_EQ(Ticks, 2);
	Preview->Tick({}); EXPECT_EQ(Ticks, 3);
	auto* Game = MakeWorld(); ASSERT_NO_THROW(Game->InitializeSubsystems());
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
	auto* PIE = MakeWorld(EWorldType::PIE);
	auto* Preview = MakeWorld(EWorldType::EditorPreview);
	for (auto* World : {Editor, PIE, Preview}) ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* Copy = DuplicateObject(Editor, nullptr, "Copy", &Copies).value();
	ASSERT_NE(Copy, nullptr); Worlds.emplace_back(Copy);
	EXPECT_EQ(Copy->GetSubsystem<DCollisionDebugSubsystem>(), nullptr);
	EXPECT_EQ(Copies.size(), 1u);
	PIE->Shutdown(); Preview->Shutdown();
	EXPECT_TRUE(Editor->IsCollisionDebugDrawEnabled());
	Editor->Shutdown(); EXPECT_FALSE(Editor->IsCollisionDebugDrawEnabled());
}

TEST_F(FWorldSubsystemTests, ProviderRequiresFrozenWorldShutdownAndOwnsRegistrationCleanup)
{
	class FProvider : public IModuleInterface
	{
	public:
		auto SupportsDynamicReloading() const -> bool override { return true; }
		auto StartupModule() -> void override
		{
			Registration = std::make_unique<FWorldSubsystemRegistration>(FWorldSubsystemDescriptor{
				.Type = FSubsystemProbeA::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
		}
		auto CanShutdownModule() const -> bool override { return !World || World->GetSubsystemState() == EWorldSubsystemState::Shutdown; }
		auto ShutdownModule() -> void override { require(CanShutdownModule()); Registration.reset(); }
		DWorld* World = nullptr;
		std::unique_ptr<FWorldSubsystemRegistration> Registration;
	};
	auto* Provider = static_cast<FProvider*>(FModuleTestHarness::InstallStartedModule("WorldSubsystemFixtureProvider", std::make_unique<FProvider>()));
	ASSERT_NE(nullptr, Provider);
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	Provider->World = World;
	auto Cancellation = World->GetSubsystem<FSubsystemProbeA>()->Cancellation;
	auto& Manager = FModuleManager::Get();
	EXPECT_FALSE(Manager.UnloadModule("WorldSubsystemFixtureProvider"));
	EXPECT_TRUE(Manager.IsModuleLoaded("WorldSubsystemFixtureProvider"));
	World->Shutdown();
	EXPECT_TRUE(Cancellation->IsCanceled());
	// Probe classes live in the test executable, so this fixture can release its logical module.
	EXPECT_TRUE(Manager.UnloadModule("WorldSubsystemFixtureProvider"));
	CollectGarbage();
	FWorldSubsystemRegistration Stale({.Type = FSubsystemProbeA::StaticClass(), .Provider = "WorldSubsystemFixtureProvider"});
	EXPECT_DEATH(MakeWorld()->InitializeSubsystems(), "Subsystem provider must be active");
}

TEST_F(FWorldSubsystemTests, LateDetachedCompletionCannotPublishAfterWorldRetirement)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	auto Cancellation = World->GetSubsystem<FSubsystemProbeA>()->Cancellation;
	struct FDetachedPayload { std::mutex Mutex; std::condition_variable Condition; bool bReleased = false; int Value = 0; };
	auto Payload = std::make_shared<FDetachedPayload>();
	auto Worker = Durin::Tasks::FTask(Tasks::LaunchTask("SubsystemDetachedWork", [Payload] {
		std::unique_lock Lock(Payload->Mutex);
		Payload->Condition.wait(Lock, [&] { return Payload->bReleased; });
		Payload->Value = 42;
	}));
	World->Shutdown();
	{ std::lock_guard Lock(Payload->Mutex); Payload->bReleased = true; }
	Payload->Condition.notify_all();
	EXPECT_EQ(WaitTask(Worker).WaitStatus, ETaskWaitStatus::Completed);
	int Published = 0;
	// This is the owner's GameThread completion boundary; no retired object is resolved.
	if (!Cancellation->IsCanceled()) Published = Payload->Value;
	EXPECT_EQ(Published, 0);
	EXPECT_TRUE(Cancellation->IsCanceled());
}

TEST_F(FWorldSubsystemTests, InitializationRetainsUnpublishedWorldDuringCollection)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FSubsystemProbeA::Callback = [](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init") { CollectGarbage(); EXPECT_FALSE(Object.IsPendingKill()); EXPECT_NE(Object.GetWorld(), nullptr); }
	};
	auto* World = NewObject<DWorld>(nullptr, "UnpublishedWorld");
	ASSERT_NO_THROW(World->InitializeSubsystems());
	Worlds.emplace_back(World);
	EXPECT_NE(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
}

TEST_F(FWorldSubsystemTests, TickEndPlayStopsLaterServicesAndRetainsThemForAnotherPlay)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass(), .bTick = true});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass(), .bTick = true});
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
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
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
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

TEST_F(FWorldSubsystemTests, WrongScopeAndMissingRequiredDependenciesAreContractViolations)
{
	FSubsystemProbeA::Dependencies[FSubsystemProbeA::StaticClass()] = {FSubsystemProbeB::StaticClass()};
	{
		FWorldSubsystemRegistration Invalid({.Type = DObject::StaticClass()});
		EXPECT_DEATH(MakeWorld()->InitializeSubsystems(), "concrete service in scope");
	}
	{
		FWorldSubsystemRegistration Missing({.Type = FSubsystemProbeA::StaticClass()});
		EXPECT_DEATH(MakeWorld()->InitializeSubsystems(), "Required subsystem dependency");
	}
}

TEST_F(FWorldSubsystemTests, ExceptionRollsBackFailedObjectBeforeDependencies)
{
	FSubsystemProbeA::Dependencies[FSubsystemProbeB::StaticClass()] = {FSubsystemProbeA::StaticClass()};
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	std::vector<DClass*> Cleanup;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init" && Object.GetClass() == FSubsystemProbeB::StaticClass()) throw std::runtime_error("fixture");
		if (Event == "Deinit") Cleanup.push_back(Object.GetClass());
	};
	auto* World = MakeWorld();
	EXPECT_THROW(World->InitializeSubsystems(), std::exception);
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_EQ(Cleanup, (std::vector<DClass*>{FSubsystemProbeB::StaticClass(), FSubsystemProbeA::StaticClass()}));
}

TEST_F(FWorldSubsystemTests, ShutdownDuringInitializeDefersCleanupUntilCallbackReturns)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	bool bInsideInitialize = false;
	int Cleanup = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init") {
			bInsideInitialize = true;
			Object.GetWorld()->Shutdown();
			EXPECT_TRUE(Object.GetWorld()->IsShutdownRequested());

			EXPECT_EQ(Cleanup, 0);
			bInsideInitialize = false;
		}
		if (Event == "Deinit") { EXPECT_FALSE(bInsideInitialize); ++Cleanup; }
	};
	auto* World = MakeWorld();
	EXPECT_NO_THROW(World->InitializeSubsystems());
	EXPECT_EQ(Cleanup, 1);
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_DEATH(World->InitializeSubsystems(), "one-shot");
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
		auto StartEngine() -> void { return InitializeEngineSubsystems(); }
		auto StartEditor() -> void { return FEditorEngineTestAccess::StartEditor(*this); }
		auto Dispatch(const std::function<void()>& Callback) -> void { FHostOperationScope Operation(*this); Callback(); }
		auto RequestSubsystemDeinitialization() -> void override
		{
			++ShutdownRequests;
			if (OnShutdownRequested) OnShutdownRequested();
			Super::RequestSubsystemDeinitialization();
		}
		int ShutdownRequests = 0;
		std::function<void()> OnShutdownRequested;
	};
	class FEngineSubsystemProbe : public DEngineSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FEngineSubsystemProbe, DEngineSubsystem, FEngineSubsystemProbe::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FEngineSubsystemProbe)
	public:
		explicit FEngineSubsystemProbe(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) { if (ConstructorCallback) ConstructorCallback(*this); }
		auto Initialize(FSubsystemCollection& Collection) -> void override
		{
			for (auto* Type : FSubsystemProbeA::Dependencies[GetClass()]) Collection.InitializeDependency(Type);
			if (Callback) Callback(*this, true);
		}
		auto Deinitialize() noexcept -> void override { if (Callback) Callback(*this, false); }
		auto ShouldCreateSubsystem(DObject* Outer) const -> bool override { return !Create || Create(*this, Outer); }
		inline static std::function<bool(const FEngineSubsystemProbe&, DObject*)> Create;
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
		auto Initialize(FSubsystemCollection&) -> void override { if (Callback) Callback(*this, true); }
		auto Deinitialize() noexcept -> void override { if (Callback) Callback(*this, false); }
		inline static std::function<void(FEditorSubsystemProbe&, bool)> Callback;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FSubsystemEditorHost)
	auto FEngineSubsystemProbe::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FEngineSubsystemProbe", Class, nullptr, sizeof(FEngineSubsystemProbe), alignof(FEngineSubsystemProbe),
				EClassFlags::None, InternalConstructor<FEngineSubsystemProbe>, &DEngineSubsystem::StaticClass);
		return Class;
	}
	auto FEditorSubsystemProbe::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FEditorSubsystemProbe", Class, nullptr, sizeof(FEditorSubsystemProbe), alignof(FEditorSubsystemProbe),
				EClassFlags::None, InternalConstructor<FEditorSubsystemProbe>, &DEditorSubsystem::StaticClass);
		return Class;
	}
}

namespace Durin
{
	auto CreateSubsystemProbeDefaults() -> bool
	{
		const std::array Classes{FSubsystemProbeA::StaticClass(), FSubsystemProbeB::StaticClass(),
			FEngineSubsystemProbe::StaticClass(), FEditorSubsystemProbe::StaticClass()};
		for (auto* Class : Classes) DObjectForceRegistration(Class);
		return Private::CreateClassDefaultObjectsForBatch(Classes);
	}
}

TEST_F(FWorldSubsystemTests, UninitializedSubsystemGcDoesNotRetireHosts)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FEditorSubsystemRegistration EditorToken({.Type = FEditorSubsystemProbe::StaticClass()});
	FWorldSubsystemRegistration WorldToken({.Type = FSubsystemProbeA::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());


	int Cleanups = 0;
	FEngineSubsystemProbe::Callback = [&](auto&, bool bInit) { if (!bInit) ++Cleanups; };
	FEditorSubsystemProbe::Callback = [&](auto&, bool bInit) { if (!bInit) ++Cleanups; };
	FSubsystemProbeA::Callback = [&](auto&, std::string_view Event) { if (Event == "Deinit") ++Cleanups; };
	// These objects never entered a collection and have no host resources to clean up.
	TWeakObjectPtr<FEngineSubsystemProbe> EngineObject(NewObject<FEngineSubsystemProbe>(Host.Get(), "UninitializedEngineService"));
	TWeakObjectPtr<FEditorSubsystemProbe> EditorObject(NewObject<FEditorSubsystemProbe>(Host.Get(), "UninitializedEditorService"));
	TWeakObjectPtr<FSubsystemProbeA> WorldObject(NewObject<FSubsystemProbeA>(World, "UninitializedWorldService"));
	MarkAsGarbage(EngineObject.Get()); MarkAsGarbage(EditorObject.Get()); MarkAsGarbage(WorldObject.Get());
	CollectGarbage();
	EXPECT_FALSE(EngineObject.IsValid()); EXPECT_FALSE(EditorObject.IsValid()); EXPECT_FALSE(WorldObject.IsValid());
	EXPECT_EQ(0, Cleanups);
	EXPECT_EQ(ESubsystemState::Ready, Host->GetSubsystemState());
	EXPECT_EQ(ESubsystemState::Ready, Host->GetEditorSubsystemState());
	EXPECT_EQ(ESubsystemState::Ready, World->GetSubsystemState());

	World->Shutdown(); Host->PrepareForShutdown();
	EXPECT_EQ(3, Cleanups);

	FEngineSubsystemProbe::Callback = {}; FEditorSubsystemProbe::Callback = {};
}

TEST_F(FWorldSubsystemTests, DeinitializedEngineAndWorldObjectsDoNotShutDownTheirHostsDuringGc)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	FSubsystemCollection EngineCollection(*Host, DEngineSubsystem::StaticClass());
	FSubsystemCollection WorldCollection(*World, DWorldSubsystem::StaticClass());
	ASSERT_NO_THROW(EngineCollection.Initialize({{.Type = FEngineSubsystemProbe::StaticClass()}}));
	ASSERT_NO_THROW(WorldCollection.Initialize({{.Type = FSubsystemProbeA::StaticClass()}}));
	TStrongObjectPtr<DSubsystem> EngineService(EngineCollection.Find(FEngineSubsystemProbe::StaticClass()));
	TStrongObjectPtr<DSubsystem> WorldService(WorldCollection.Find(FSubsystemProbeA::StaticClass()));
	TWeakObjectPtr<DSubsystem> WeakEngine(EngineService.Get()), WeakWorld(WorldService.Get());
	EngineCollection.Deinitialize(); WorldCollection.Deinitialize();
	EXPECT_TRUE(IsValid(EngineService.Get())); EXPECT_TRUE(IsValid(WorldService.Get()));
	EXPECT_FALSE(EngineService->IsGarbage()); EXPECT_FALSE(WorldService->IsGarbage());
	CollectGarbage(); EXPECT_TRUE(WeakEngine.IsValid()); EXPECT_TRUE(WeakWorld.IsValid());
	EngineService.Reset(); WorldService.Reset(); CollectGarbage();
	EXPECT_FALSE(WeakEngine.IsValid()); EXPECT_FALSE(WeakWorld.IsValid());
	EXPECT_EQ(ESubsystemState::Ready, Host->GetSubsystemState());
	EXPECT_EQ(EWorldSubsystemState::Ready, World->GetSubsystemState());
	Host->PrepareForShutdown(); MarkObjectHierarchyAsGarbage(Host.Get());
}

TEST_F(FWorldSubsystemTests, EngineEligibilityRunsOnCdoBeforeConstruction)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	int Constructions = 0;
	FEngineSubsystemProbe::ConstructorCallback = [&](auto&) { ++Constructions; };
	FEngineSubsystemProbe::Create = [&](const auto& Default, DObject* Outer) {
		EXPECT_TRUE(Default.IsClassDefaultObject());
		EXPECT_EQ(Outer, Host.Get());
		return false;
	};
	EXPECT_NO_THROW(Host->StartEngine());
	FEngineSubsystemProbe::Create = {};
	FEngineSubsystemProbe::ConstructorCallback = {};
	EXPECT_EQ(Constructions, 0);
	EXPECT_EQ(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	Host->PrepareForShutdown();
	MarkObjectHierarchyAsGarbage(Host.Get());
}

TEST_F(FWorldSubsystemTests, PlayInitializationExceptionCleansUnpublishedWorldAndAllowsRetry)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	auto* World = NewObject<DWorld>(Host.Get(), "PlayExceptionEditorWorld");
	Worlds.emplace_back(World);
	ASSERT_TRUE(World->SetWorldType(EWorldType::Editor));
	World->InitializeSubsystems();
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "EditorLevel")));
	Host->SetEditorWorld(World);
	FWorldSubsystemRegistration Throwing({.Type = FSubsystemProbeA::StaticClass()});
	TWeakObjectPtr<DWorld> Candidate;
	FSubsystemProbeA::Callback = [&](auto& Service, std::string_view Event) {
		if (Event == "Init") { Candidate = Service.GetWorld(); throw std::runtime_error("Play callback exception"); }
	};
	EXPECT_THROW(Host->StartPlay(World->GetCurrentLevel()), std::runtime_error);
	EXPECT_FALSE(Candidate.IsValid());
	EXPECT_EQ(Host->GetPlayState(), Editor::EPlayState::Stopped);
	EXPECT_EQ(Host->GetPlayWorld(), nullptr);
	EXPECT_EQ(Host->GetWorld(), World);
	EXPECT_EQ(Host->GetWorldContexts().size(), 1u);
	FSubsystemProbeA::Callback = {};
	ASSERT_TRUE(Host->StartPlay(World->GetCurrentLevel()));
	Host->StopPlaySession();
	Host->PrepareForShutdown();
}

TEST_F(FWorldSubsystemTests, OneEditorKeepsServicesAcrossEditorPlayAndPreviewWorldContexts)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FEditorSubsystemRegistration EditorToken({.Type = FEditorSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> First(NewObject<FSubsystemEditorHost>(nullptr, {}));
	TStrongObjectPtr<FSubsystemEditorHost> Second(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(First->StartEngine());
	ASSERT_NO_THROW(First->StartEditor());
	auto* EngineService = First->GetSubsystem<FEngineSubsystemProbe>();
	auto* EditorService = First->GetEditorSubsystem<FEditorSubsystemProbe>();
	auto* Notifications = &First->GetNotificationManager();
	ASSERT_NE(EngineService, nullptr); ASSERT_NE(EditorService, nullptr);
	EXPECT_EQ(EngineService->GetEngine(), First.Get());
	EXPECT_EQ(EditorService->GetEditor(), First.Get());
	EXPECT_EQ(Second->GetSubsystem<FEngineSubsystemProbe>(), nullptr); EXPECT_EQ(GEditor, First.Get());
	auto* EditorWorld = NewObject<DWorld>(First.Get(), {});
	Worlds.emplace_back(EditorWorld);
	EditorWorld->SetWorldType(EWorldType::Editor);
	ASSERT_NO_THROW(EditorWorld->InitializeSubsystems());
	ASSERT_TRUE(EditorWorld->SetCurrentLevel(NewObject<DLevel>(EditorWorld, "EditorLevel")));
	First->SetEditorWorld(EditorWorld);
	EXPECT_EQ(First->GetEditorWorldContext().World(), EditorWorld);
	EXPECT_EQ(First->GetEditorWorldContext().WorldType, EWorldType::Editor);
	auto* PreviewWorld = NewObject<DWorld>(First.Get(), "ContextPreviewWorld");
	Worlds.emplace_back(PreviewWorld); PreviewWorld->SetWorldType(EWorldType::EditorPreview);
	EXPECT_EQ(First->GetWorldContextFromWorld(PreviewWorld), nullptr);
	ASSERT_NO_THROW(PreviewWorld->InitializeSubsystems());
	ASSERT_NE(First->GetWorldContextFromWorld(PreviewWorld), nullptr);
	EXPECT_EQ(First->GetWorldContextFromWorld(PreviewWorld)->WorldType, EWorldType::EditorPreview);
	for (int Session = 0; Session < 2; ++Session) {
		ASSERT_TRUE(First->StartPlay(EditorWorld->GetCurrentLevel()));
		EXPECT_NE(First->GetPlayWorld(), nullptr);
		ASSERT_NE(First->GetPIEWorldContext(), nullptr);
		EXPECT_EQ(First->GetPIEWorldContext()->World(), First->GetPlayWorld());
		EXPECT_EQ(First->GetWorldContexts().size(), 3u);
		First->StopPlaySession();
		EXPECT_EQ(First->GetPIEWorldContext(), nullptr);
		EXPECT_EQ(First->GetWorld(), EditorWorld);
		EXPECT_EQ(First->GetSubsystem<FEngineSubsystemProbe>(), EngineService);
		EXPECT_EQ(First->GetEditorSubsystem<FEditorSubsystemProbe>(), EditorService);
		EXPECT_EQ(&First->GetNotificationManager(), Notifications);
	}
	CollectGarbage();
	EXPECT_EQ(First->GetSubsystem<FEngineSubsystemProbe>(), EngineService);
	EXPECT_EQ(First->GetEditorSubsystem<FEditorSubsystemProbe>(), EditorService);
	PreviewWorld->Shutdown();
	EXPECT_EQ(First->GetWorldContextFromWorld(PreviewWorld), nullptr);
	First->PrepareForShutdown(); First->PrepareForShutdown();
	EXPECT_EQ(First->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	ASSERT_NO_THROW(Second->StartEngine());
	ASSERT_NO_THROW(Second->StartEditor()); EXPECT_EQ(GEditor, Second.Get());
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
			EXPECT_TRUE(Service.GetEngine()->IsShutdownRequested());

			EXPECT_EQ(Cleanups, 0);
		} else ++Cleanups;
	};
	EXPECT_NO_THROW(Host->StartEngine());
	EXPECT_EQ(Cleanups, 1);
	FEngineSubsystemProbe::Callback = {};
	TStrongObjectPtr<FSubsystemEditorHost> Ready(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Ready->StartEngine());

	Ready->Dispatch([&] {
		Ready->PrepareForShutdown();
		EXPECT_TRUE(Ready->IsShutdownRequested());
		EXPECT_NE(Ready->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
		CollectGarbage();
	});
	EXPECT_EQ(Ready->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
}

TEST_F(FWorldSubsystemTests, ReentrantShutdownRequestsNotifyOnceAndRetireAfterTheHostOperation)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	int Cleanups = 0, ExternalCalls = 0;
	FEngineSubsystemProbe::Callback = [&](auto&, bool bInit) {
		if (bInit) return;
		++Cleanups;
		Host->PrepareForShutdown();
		EXPECT_EQ(1, Host->ShutdownRequests);
	};
	Host->OnShutdownRequested = [&] {
		Host->PrepareForShutdown();
		EXPECT_EQ(1, Host->ShutdownRequests);
		EXPECT_NE(nullptr, Host->GetSubsystem<FEngineSubsystemProbe>());
		EXPECT_EQ(0, Cleanups);
	};
	Host->Dispatch([&] {
		Host->PrepareForShutdown(); Host->PrepareForShutdown();
		Host->DispatchSubsystemCallback([&] { ++ExternalCalls; });
		EXPECT_EQ(1, Host->ShutdownRequests);
		EXPECT_EQ(0, Cleanups); EXPECT_EQ(0, ExternalCalls);
	});
	EXPECT_EQ(1, Cleanups);
	EXPECT_EQ(ESubsystemState::Shutdown, Host->GetSubsystemState());
	Host->PrepareForShutdown();
	EXPECT_EQ(1, Host->ShutdownRequests);
	Host->OnShutdownRequested = {}; FEngineSubsystemProbe::Callback = {};
}

TEST_F(FWorldSubsystemTests, WorldAndEditorCleanupRetainEngineDependencies)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FEditorSubsystemRegistration EditorToken({.Type = FEditorSubsystemProbe::StaticClass()});
	FWorldSubsystemRegistration WorldToken({.Type = FSubsystemProbeA::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	auto* World = NewObject<DWorld>(Host.Get(), {}); Worlds.emplace_back(World);
	ASSERT_NO_THROW(World->InitializeSubsystems()); Host->SetWorld(World);
	auto* Preview = NewObject<DWorld>(Host.Get(), "UnselectedPreviewWorld");
	Worlds.emplace_back(Preview);
	ASSERT_TRUE(Preview->SetWorldType(EWorldType::EditorPreview));
	ASSERT_NO_THROW(Preview->InitializeSubsystems());
	int Checks = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA&, std::string_view Event) {
		if (Event == "Deinit") { EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr); ++Checks; }
	};
	FEditorSubsystemProbe::Callback = [&](FEditorSubsystemProbe&, bool bInit) {
		if (!bInit) { EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr); ++Checks; }
	};
	Host->PrepareForShutdown();
	EXPECT_EQ(Checks, 3);
	EXPECT_TRUE(Host->GetWorldContexts().empty());
	FEditorSubsystemProbe::Callback = {};
}

TEST_F(FWorldSubsystemTests, EngineAndEditorRegistrationsRejectUnrelatedTypes)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEditorSubsystemProbe::StaticClass()});
	FEditorSubsystemRegistration EditorToken({.Type = FSubsystemProbeA::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	EXPECT_DEATH(Host->StartEngine(), "concrete service in scope");
	EXPECT_DEATH(Host->StartEditor(), "concrete service in scope");
	Host->PrepareForShutdown();
}

TEST_F(FWorldSubsystemTests, EditorInitializationCanRequestShutdownOrThrow)
{
	FEditorSubsystemRegistration Token({.Type = FEditorSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	int Cleanups = 0;
	FEditorSubsystemProbe::Callback = [&](FEditorSubsystemProbe& Service, bool bInit) {
		if (bInit) {
			Service.GetEditor()->PrepareForShutdown();
			EXPECT_TRUE(Service.GetEditor()->IsShutdownRequested());

			EXPECT_EQ(Cleanups, 0);
		} else ++Cleanups;
	};
	EXPECT_NO_THROW(Host->StartEditor());
	EXPECT_EQ(Cleanups, 1);
	FEditorSubsystemProbe::Callback = {};
	TStrongObjectPtr<FSubsystemEditorHost> Failed(NewObject<FSubsystemEditorHost>(nullptr, {}));
	FEditorSubsystemProbe::Callback = [](FEditorSubsystemProbe&, bool bInit) { if (bInit) throw std::runtime_error("fixture"); };
	EXPECT_THROW(Failed->StartEditor(), std::exception);
	EXPECT_EQ(Failed->GetEditorSubsystem<FEditorSubsystemProbe>(), nullptr);
	FEditorSubsystemProbe::Callback = {};
	Failed->PrepareForShutdown();
}

TEST_F(FWorldSubsystemTests, NotificationServiceOwnsFacadeAndRejectsRetainedActionsAfterShutdown)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	auto* Service = Host->GetEditorSubsystem<DEditorNotificationSubsystem>();
	ASSERT_NE(Service, nullptr);
	EXPECT_EQ(&Host->GetNotificationManager(), &Service->GetManager());
	int Invocations = 0;
	Host->GetNotificationManager().Post({.Message = "Ready", .Action = Editor::FNotificationAction{
		.Label = "Open", .Invoke = [&] { ++Invocations; }}});
	Host->GetNotificationManager().Tick(0.0f);
	ASSERT_EQ(Service->GetManager().GetNotifications().size(), 1u);
	auto Action = Service->GetManager().GetNotifications().front().Action;

	Host->PrepareForShutdown();

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
	ASSERT_NO_THROW(Host->StartEngine());
	auto* World = MakeWorld(EWorldType::EditorPreview);
	ASSERT_NO_THROW(World->InitializeSubsystems());
	Host->SetWorld(World);
	int Ticks = 0;
	bool bInsideCallback = false;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Service, std::string_view Event) {
		if (Event == "Tick") {
			++Ticks;
			bInsideCallback = true;
			Host->PrepareForShutdown();
			EXPECT_TRUE(World->IsShutdownRequested());
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

TEST_F(FWorldSubsystemTests, ConstructorRetirementDoesNotEnterInitialize)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	int Initializations = 0;
	int Cleanups = 0;
	FEngineSubsystemProbe::ConstructorCallback = [&](FEngineSubsystemProbe&) { Host->PrepareForShutdown(); };
	FEngineSubsystemProbe::Callback = [&](FEngineSubsystemProbe& Service, bool bInit) {
		if (bInit) ++Initializations;
		else { ++Cleanups; }
	};
	EXPECT_NO_THROW(Host->StartEngine());
	EXPECT_EQ(Initializations, 0);
	EXPECT_EQ(Cleanups, 1);
	FEngineSubsystemProbe::ConstructorCallback = {};
	FEngineSubsystemProbe::Callback = {};
}

TEST_F(FWorldSubsystemTests, NotificationShutdownCallbackRetiresAtTheNextHostBoundary)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	auto* Service = Host->GetEditorSubsystem<DEditorNotificationSubsystem>();
	ASSERT_NE(Service, nullptr);
	const auto Id = Host->GetNotificationManager().Post({.Message = "Stop", .Action = Editor::FNotificationAction{
		.Label = "Stop", .Invoke = [&] {
			Host->PrepareForShutdown();
			EXPECT_FALSE(Service->GetManager().IsAcceptingRequests());
			EXPECT_EQ(0u, Service->GetManager().Post({.Message = "Too late"}));
			EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), Service);
			EXPECT_EQ(Service->GetManager().GetNotifications().size(), 1u);
			CollectGarbage();
			EXPECT_FALSE(Service->IsPendingKill());
		}}});
	Host->GetNotificationManager().Tick(0.0f);
	EXPECT_TRUE(Host->GetNotificationManager().InvokeAction(Id));
	EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), Service);
	Host->Tick(0.0f, false);
	EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), nullptr);
}

TEST_F(FWorldSubsystemTests, HostGarbageFallbackRetiresWorldAndServiceCollections)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FEditorSubsystemRegistration EditorToken({.Type = FEditorSubsystemProbe::StaticClass()});
	FWorldSubsystemRegistration WorldToken({.Type = FSubsystemProbeA::StaticClass()});
	TWeakObjectPtr<FSubsystemEditorHost> WeakHost;
	TWeakObjectPtr<DWorld> WeakWorld;
	std::vector<std::string> CleanupOrder;
	{
		TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
		ASSERT_NO_THROW(Host->StartEngine());
		ASSERT_NO_THROW(Host->StartEditor());


		auto* World = NewObject<DWorld>(Host.Get(), "GcFallbackWorld");
		ASSERT_NO_THROW(World->InitializeSubsystems()); Host->SetEditorWorld(World);

		WeakHost = Host.Get(); WeakWorld = World;
		FEngineSubsystemProbe::Callback = [&](auto&, bool bInit) { if (!bInit) CleanupOrder.emplace_back("engine"); };
		FEditorSubsystemProbe::Callback = [&](auto&, bool bInit) { if (!bInit) CleanupOrder.emplace_back("editor"); };
		FSubsystemProbeA::Callback = [&](auto&, std::string_view Event) { if (Event == "Deinit") CleanupOrder.emplace_back("world"); };
	}
	CollectGarbage();
	EXPECT_EQ((std::vector<std::string>{"world", "editor", "engine"}), CleanupOrder);


	EXPECT_FALSE(WeakHost.IsValid()); EXPECT_FALSE(WeakWorld.IsValid());
	FEngineSubsystemProbe::Callback = {}; FEditorSubsystemProbe::Callback = {};
}

TEST_F(FWorldSubsystemTests, WrongScopeDependencyIsRejectedDuringInitialization)
{
	FSubsystemProbeA::Dependencies[FEngineSubsystemProbe::StaticClass()] = {FEditorSubsystemProbe::StaticClass()};
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	EXPECT_DEATH(Host->StartEngine(), "concrete service in scope");
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
	ASSERT_NO_THROW(Host->StartEngine());
	auto* World = NewObject<DWorld>(Host.Get(), "UnpublishedWorld");
	Worlds.emplace_back(World);
	int Initializations = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Service, std::string_view Event) {
		if (Event != "Init") return;
		++Initializations;
		EXPECT_EQ(Host->GetWorld(), nullptr);
		Host->PrepareForShutdown();
		EXPECT_TRUE(Service.GetWorld()->IsShutdownRequested());
		EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	};
	EXPECT_NO_THROW(World->InitializeSubsystems());
	EXPECT_EQ(Initializations, 1);
	EXPECT_EQ(Host->GetWorld(), nullptr);
	EXPECT_EQ(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
}

TEST_F(FWorldSubsystemTests, ActorEditingAdmitsOnlyItsLiveHostAndEditableStoppedDocument)
{
	using namespace Durin::Editor;
	TStrongObjectPtr<FSubsystemEditorHost> First(NewObject<FSubsystemEditorHost>(nullptr, {}));
	for (auto* Host : {First.Get()})
	{
		ASSERT_NO_THROW(Host->StartEngine());
		ASSERT_NO_THROW(Host->StartEditor());
		auto* World = NewObject<DWorld>(Host, {});
		Worlds.emplace_back(World);
		World->SetWorldType(EWorldType::Editor);
		ASSERT_NO_THROW(World->InitializeSubsystems());
		ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "EditorDocumentLevel")));
		Host->SetEditorWorld(World);
	}
	auto* Level = First->GetEditorWorld()->GetCurrentLevel();
	auto* Service = First->GetEditorSubsystem<DEditorActorSubsystem>();
	ASSERT_NE(Service, nullptr);
	TStrongObjectPtr<DEditorActorSubsystem> Retained(Service);
	const FActorEditContext Context{false};
	EXPECT_FALSE(Service->SpawnActor(*Level, AActor::StaticClass(), "ReadOnly", {}, {true}));
	EXPECT_EQ(Service->GetEditor(), First.Get());
	auto* OtherWorld = NewObject<DWorld>(First.Get(), "OtherDocumentWorld"); Worlds.emplace_back(OtherWorld);
	OtherWorld->SetWorldType(EWorldType::EditorPreview); ASSERT_NO_THROW(OtherWorld->InitializeSubsystems());
	ASSERT_TRUE(OtherWorld->SetCurrentLevel(NewObject<DLevel>(OtherWorld, "OtherDocumentLevel")));
	EXPECT_FALSE(Service->SpawnActor(*OtherWorld->GetCurrentLevel(), AActor::StaticClass(), "ForeignLevel", {}, Context));
	const auto Pending = First->GetTransactor()->Begin("test", "Grouped host history");
	ASSERT_GE(Pending, 0);
	const auto Grouped = Service->SpawnActor(*Level, AActor::StaticClass(), "Grouped", {}, Context);
	ASSERT_TRUE(Grouped);
	EXPECT_EQ(First->GetTransactor()->GetUndoId(), 0u);
	ASSERT_TRUE(First->GetTransactor()->End());
	EXPECT_EQ(First->GetTransactor()->GetUndoContext(false).Description, "Grouped host history");
	ASSERT_TRUE(First->GetTransactor()->Undo()) << FormatTransactorResult(First->GetTransactor()->GetLastResult());
	EXPECT_FALSE(Level->ContainsActor(Grouped));
	ASSERT_TRUE(First->GetTransactor()->Redo());
	EXPECT_TRUE(Level->ContainsActor(Grouped));
	const auto Added = Service->SpawnActor(*Level, AActor::StaticClass(), "Edited", {}, Context);
	ASSERT_TRUE(Added);
	const auto BeforeNoOp = First->GetTransactor()->GetUndoId();
	const auto SameName = Service->RenameActor(*Level, Added, "Edited", Context);
	EXPECT_TRUE(SameName);
	EXPECT_EQ(First->GetTransactor()->GetUndoId(), BeforeNoOp);
	ASSERT_TRUE(First->StartPlay(Level));
	EXPECT_FALSE(Service->RenameActor(*Level, Added, "Playing", Context));
	First->StopPlaySession();
	First->PrepareForShutdown();
	EXPECT_FALSE(Service->SpawnActor(*Level, AActor::StaticClass(), "Retired", {}, Context));
}

TEST_F(FWorldSubsystemTests, ActorPreparationCannotPublishAfterItsHostClosesAdmission)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	auto* World = NewObject<DWorld>(Host.Get(), {});
	Worlds.emplace_back(World);
	World->SetWorldType(EWorldType::Editor);
	ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "EditorDocumentLevel")));
	Host->SetEditorWorld(World);
	TStrongObjectPtr<DLevel> Level(World->GetCurrentLevel());
	auto* Service = Host->GetEditorSubsystem<DEditorActorSubsystem>();
	ASSERT_NE(Service, nullptr);
	AActor* Prepared = nullptr;
	const auto Result = Service->SpawnActor(*Level, AActor::StaticClass(), "RetiringPreparation", {}, {false}, [&](AActor& Actor) {
		Prepared = &Actor;
		Host->PrepareForShutdown();
	});
	EXPECT_FALSE(Result);
	EXPECT_FALSE(Result);
	EXPECT_FALSE(IsValid(Prepared));
	EXPECT_FALSE(Level->FindActorByName("RetiringPreparation"));
}

TEST_F(FWorldSubsystemTests, StructuralActorEditingRejectsSubsystemBeginPlayBeforeActorPlayDispatch)
{
	FWorldSubsystemRegistration Registration({.Type = FSubsystemProbeA::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	auto* World = NewObject<DWorld>(Host.Get(), {});
	Worlds.emplace_back(World);
	World->SetWorldType(EWorldType::Editor);
	ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "EditorDocumentLevel")));
	Host->SetEditorWorld(World);
	auto* Level = World->GetCurrentLevel();
	auto* Actor = Level->SpawnActor<ACameraActor>("LifecycleOwner");
	auto* Service = Host->GetEditorSubsystem<DEditorActorSubsystem>();
	ASSERT_NE(Service, nullptr);
	ASSERT_NE(Service->AddComponent(*Actor, DSceneComponent::StaticClass(), nullptr, {false}), nullptr);
	const auto UndoId = Host->GetTransactor()->GetUndoId();
	int Calls = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA&, std::string_view Event) {
		if (Event != "Begin") return;
		++Calls;
		EXPECT_FALSE(World->HasBegunPlay());
		EXPECT_EQ(World->GetPlayState(), EWorldPlayState::BeginningPlay);
		EXPECT_FALSE(Service->SpawnActor(*Level, AActor::StaticClass(), "Beginning", {}, {false}));
		EXPECT_FALSE(Host->GetTransactor()->Undo());
		EXPECT_EQ(Host->GetTransactor()->GetUndoId(), UndoId);
	};
	ASSERT_TRUE(World->BeginPlay({}));
	EXPECT_EQ(Calls, 1);
	World->EndPlay();
	ASSERT_TRUE(Host->GetTransactor()->Undo());
	Host->PrepareForShutdown();
}

TEST_F(FWorldSubsystemTests, StructuralActorEditingRejectsWorldShutdownRequestBeforeTeardown)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	ASSERT_NO_THROW(Host->StartEditor());
	auto* World = NewObject<DWorld>(Host.Get(), {});
	Worlds.emplace_back(World);
	World->SetWorldType(EWorldType::Editor);
	ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "EditorDocumentLevel")));
	Host->SetEditorWorld(World);
	auto* Level = World->GetCurrentLevel();
	auto* Actor = Level->SpawnActor<ACameraActor>("ClosingOwner");
	auto* Service = Host->GetEditorSubsystem<DEditorActorSubsystem>();
	ASSERT_NE(Service, nullptr);
	ASSERT_NE(Service->AddComponent(*Actor, DSceneComponent::StaticClass(), nullptr, {false}), nullptr);
	const auto UndoId = Host->GetTransactor()->GetUndoId();
	World->RequestShutdown();
	EXPECT_EQ(World->GetCurrentLevel(), Level);
	EXPECT_TRUE(World->IsShutdownRequested());
	EXPECT_FALSE(Service->SpawnActor(*Level, AActor::StaticClass(), "Closing", {}, {false}));
	EXPECT_FALSE(Host->GetTransactor()->Undo());
	EXPECT_EQ(Host->GetTransactor()->GetUndoId(), UndoId);
	Host->PrepareForShutdown();
}
