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
	template<typename T>
	concept CHasSubsystemTick = requires(T& Object) { Object.Tick(0.0f); };
	static_assert(!CHasSubsystemTick<DWorldSubsystem>);
	static_assert(CHasSubsystemTick<DTickableWorldSubsystem>);

	auto CreateSubsystemProbeDefaults() -> bool;
	// Native fixtures exercise the same class factory path as registered module services.
	class FSubsystemProbeA : public DTickableWorldSubsystem
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FSubsystemProbeA, DTickableWorldSubsystem, FSubsystemProbeA::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FSubsystemProbeA)
	public:
		explicit FSubsystemProbeA(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer) { if (!IsClassDefaultObject()) ++InstanceConstructions; }
		inline static int InstanceConstructions = 0;
		inline static bool bFailBeforeSuper = false;
		inline static std::map<DClass*, std::vector<DClass*>> Dependencies;
		auto Initialize(FSubsystemCollection& Collection) -> void override
		{
			if (bFailBeforeSuper) throw std::runtime_error("before Super");
			Super::Initialize(Collection);
			for (auto* Type : Dependencies[GetClass()])
				if (!Collection.InitializeDependency(Type))
				{
					if (GetWorld()->IsShutdownRequested()) return;
					requiref(false, "Required subsystem dependency is unavailable.");
				}
			Event("Init");
			if (Failure) throw std::runtime_error("fixture");
		}
		auto Deinitialize() noexcept -> void override { Super::Deinitialize(); Cancellation->Cancel(); Event("Deinit"); }
		std::shared_ptr<Tasks::FCancellationToken> Cancellation = std::make_shared<Tasks::FCancellationToken>();
		auto OnWorldBeginPlay() noexcept -> void override { Event("Begin"); }
		auto OnWorldEndPlay() noexcept -> void override { Event("End"); }
		auto OnLevelAttached(DLevel&) noexcept -> void override { Event("Attach"); }
		auto OnLevelDetached(DLevel&) noexcept -> void override { Event("Detach"); }
		bool bTickable = true;
		bool bTickInEditor = false;
		bool bTickWhenPaused = false;
		float LastDelta = -1.0f;
		auto IsTickable() const -> bool override { return bTickable; }
		auto IsTickableInEditor() const -> bool override { return bTickInEditor; }
		auto IsTickableWhenPaused() const -> bool override { return bTickWhenPaused; }
		auto Tick(float Delta) noexcept -> void override { LastDelta = Delta; Event("Tick"); }
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
	class FStoppingBootstrapGameMode : public AGameMode
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FStoppingBootstrapGameMode, AGameMode, FStoppingBootstrapGameMode::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FStoppingBootstrapGameMode)
	public:
		explicit FStoppingBootstrapGameMode(const FObjectInitializer& Initializer) : Super(Initializer) {}
		auto ChoosePlayerStart(DWorld& World) const -> APlayerStart* override
		{
			World.EndPlay();
			return Super::ChoosePlayerStart(World);
		}
	};
	auto FStoppingBootstrapGameMode::GetPrivateStaticClass() -> DClass*
	{
		static DClass* Class = nullptr;
		if (!Class)
			GetPrivateStaticClassBody("", "FStoppingBootstrapGameMode", Class, nullptr, sizeof(FStoppingBootstrapGameMode), alignof(FStoppingBootstrapGameMode),
				EClassFlags::None, InternalConstructor<FStoppingBootstrapGameMode>, &AGameMode::StaticClass);
		return Class;
	}
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
				EClassFlags::None, InternalConstructor<FSubsystemProbeA>, &DTickableWorldSubsystem::StaticClass);
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
			bPreviousEditorMode = GIsEditor;
			GIsEditor = true;
			InitializeDObjectSystem();
			ASSERT_TRUE(CreateSubsystemProbeDefaults());
			FSubsystemProbeA::InstanceConstructions = 0;
			FSubsystemProbeA::Support = [](const auto&, EWorldType) { return true; };
		}
		auto TearDown() -> void override
		{
			GIsEditor = bPreviousEditorMode;
			FSubsystemProbeA::bFailBeforeSuper = false;
			FSubsystemProbeA::Callback = {};
			FSubsystemProbeA::Dependencies.clear();
			FSubsystemProbeA::Support = {};
			FSubsystemProbeA::Create = {};
			for (auto& World : Worlds) { World->CleanupWorld(); MarkObjectHierarchyAsGarbage(World.Get()); }
			Worlds.clear();
			CollectGarbage(Durin::EObjectFlags::Standalone);
		}
		auto MakeWorld(EWorldType Type = EWorldType::Game) -> DWorld*
		{
			auto* World = NewObject<DWorld>(nullptr, "SubsystemWorld");
			World->SetWorldType(Type);
			Worlds.emplace_back(World);
			return World;
		}
		bool bPreviousEditorMode = false;
		std::vector<TStrongObjectPtr<DWorld>> Worlds;
	};
}

namespace
{
	class FTickableProbe : public FTickableGameObject
	{
	public:
		explicit FTickableProbe(DWorld* InWorld = nullptr, ETickableTickType Type = ETickableTickType::NewObject)
			: FTickableGameObject(Type), World(InWorld) {}
		DWorld* World;
		bool bAllowed = true;
		bool bTickable = true;
		bool bEditor = false;
		bool bPaused = false;
		mutable int TypeQueries = 0;
		mutable int ConditionQueries = 0;
		int Ticks = 0;
		float Delta = -1.0f;
		ETickableTickType InitialType = ETickableTickType::Conditional;
		std::function<void()> Callback;
		auto GetTickableTickType() const -> ETickableTickType override { ++TypeQueries; return InitialType; }
		auto IsAllowedToTick() const -> bool override { return bAllowed; }
		auto IsTickable() const -> bool override { ++ConditionQueries; return bTickable; }
		auto IsTickableInEditor() const -> bool override { return bEditor; }
		auto IsTickableWhenPaused() const -> bool override { return bPaused; }
		auto GetTickableGameObjectWorld() const -> DWorld* override { return World; }
		auto Tick(float InDelta) noexcept -> void override
		{
			++Ticks;
			Delta = InDelta;
			auto Copy = Callback; // The callback may destroy this object.
			if (Copy) Copy();
		}
	};
}

TEST_F(FWorldSubsystemTests, GenericTickablesRouteToTheirWorldWithoutSubsystemMembership)
{
	auto* First = MakeWorld();
	auto* Second = MakeWorld();
	First->InitializeSubsystems();
	Second->InitializeSubsystems();
	FTickableProbe A(First), B(Second), Global;
	First->Tick({.DeltaSeconds = 0.25f});
	EXPECT_EQ(A.Ticks, 1);
	EXPECT_EQ(B.Ticks, 0);
	EXPECT_EQ(Global.Ticks, 0);
	Second->Tick({});
	EXPECT_EQ(A.Ticks, 1);
	EXPECT_EQ(B.Ticks, 1);
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.5f);
	EXPECT_EQ(Global.Ticks, 1);
	EXPECT_FLOAT_EQ(Global.Delta, 0.5f);
	EXPECT_FLOAT_EQ(A.Delta, 0.25f);
}

TEST_F(FWorldSubsystemTests, GenericTickTypesResolveOnceAndAlwaysStillHonorsSafetyAndPause)
{
	FTickableProbe Object;
	Object.bTickable = false;
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Object.TypeQueries, 1);
	EXPECT_EQ(Object.ConditionQueries, 2);
	EXPECT_EQ(Object.Ticks, 0);
	Object.SetTickableTickType(ETickableTickType::Always);
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 1);
	EXPECT_EQ(Object.ConditionQueries, 2);
	Object.bAllowed = false;
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 1);
	Object.bAllowed = true;
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, true, 0.0f);
	EXPECT_EQ(Object.Ticks, 1);
	Object.bPaused = true;
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, true, 0.0f);
	EXPECT_EQ(Object.Ticks, 2);
	Object.SetTickableTickType(ETickableTickType::Never);
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 2);
	Object.bTickable = true;
	Object.SetTickableTickType(ETickableTickType::Conditional);
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_ViewportsOnly, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 2);
	Object.bEditor = true;
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_ViewportsOnly, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 3);
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_TimeOnly, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 4);
}

TEST_F(FWorldSubsystemTests, GenericTickRegistrationSupportsDeletionAndDeferredAddsDuringDispatch)
{
	int AddedTicks = 0;
	int DeletedTicks = 0;
	std::unique_ptr<FTickableProbe> Added;
	auto First = std::make_unique<FTickableProbe>();
	auto Later = std::make_unique<FTickableProbe>();
	Later->Callback = [&] { ++DeletedTicks; };
	First->Callback = [&] {
		Later.reset();
		Added = std::make_unique<FTickableProbe>();
		Added->Callback = [&] { ++AddedTicks; };
		First.reset();
	};
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(AddedTicks, 0);
	EXPECT_EQ(DeletedTicks, 0);
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(AddedTicks, 1);
	Added.reset();
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(AddedTicks, 1);
}

TEST_F(FWorldSubsystemTests, GenericTickDisableAndReenableDoesNotDispatchOldRegistration)
{
	FTickableProbe First, Later;
	First.Callback = [&] {
		Later.SetTickableTickType(ETickableTickType::Never);
		Later.SetTickableTickType(ETickableTickType::Always);
	};
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Later.Ticks, 0);
	First.Callback = {};
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Later.Ticks, 1);
}

TEST_F(FWorldSubsystemTests, GenericTickNeverConstructionAndDestructionDoNotPublish)
{
	{
		FTickableProbe Object(nullptr, ETickableTickType::Never);
		FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
		EXPECT_EQ(Object.TypeQueries, 0);
		Object.SetTickableTickType(ETickableTickType::Conditional);
		FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
		EXPECT_EQ(Object.Ticks, 1);
	}
	{
		FTickableProbe Pending;
		Pending.InitialType = ETickableTickType::Never;
		FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
		EXPECT_EQ(Pending.Ticks, 0);
		EXPECT_EQ(Pending.TypeQueries, 1);
	}
	{ FTickableProbe DeletedBeforeDispatch; }
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
}

TEST_F(FWorldSubsystemTests, GenericTickQueryFailureRestoresDispatchState)
{
	class FThrowingTickable : public FTickableProbe
	{
	public:
		bool bThrow = true;
		auto IsTickable() const -> bool override
		{
			if (bThrow) throw std::runtime_error("tick query");
			return true;
		}
	} Object;
	FTickableProbe Later;
	EXPECT_THROW(FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f), std::runtime_error);
	EXPECT_EQ(Later.Ticks, 0);
	Object.bThrow = false;
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 1);
	EXPECT_EQ(Later.Ticks, 1);
}

TEST_F(FWorldSubsystemTests, GenericTickNestedDispatchIsAContractViolation)
{
	FTickableProbe Object;
	Object.Callback = [] { FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f); };
	EXPECT_DEATH(FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f), "cannot be nested");
	Object.Callback = {};
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 1);
}

TEST_F(FWorldSubsystemTests, PendingTypesResolveBeforeCallbacksAndActiveObjectsCannotBecomeNew)
{
	FTickableProbe First, Later;
	First.Callback = [&] { EXPECT_EQ(Later.TypeQueries, 1); };
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_DEATH(Later.SetTickableTickType(ETickableTickType::NewObject), "cannot return to NewObject");
}

TEST_F(FWorldSubsystemTests, EditorOptInOverridesPauseAndGameWorldIdentitySurvivesTickMode)
{
	auto* World = MakeWorld();
	World->InitializeSubsystems();
	FTickableProbe Object(World);
	Object.bEditor = true;
	FTickableGameObject::TickObjects(World, LEVELTICK_TimeOnly, true, 0.0f);
	EXPECT_EQ(Object.Ticks, 1);
	GIsEditor = false;
	FTickableGameObject::TickObjects(World, LEVELTICK_TimeOnly, true, 0.0f);
	EXPECT_EQ(Object.Ticks, 1);
	Object.bPaused = true;
	FTickableGameObject::TickObjects(World, LEVELTICK_TimeOnly, true, 0.0f);
	EXPECT_EQ(Object.Ticks, 2);
	FTickableGameObject::TickObjects(World, LEVELTICK_ViewportsOnly, false, 0.0f);
	EXPECT_EQ(Object.Ticks, 3);
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

TEST_F(FWorldSubsystemTests, InitializedTemplateSubsystemNeverRegistersForTick)
{
	auto* World = MakeWorld();
	FWorldSubsystemCollection Collection(*World);
	TStrongObjectPtr<FSubsystemProbeA> Template(NewObject<FSubsystemProbeA>(World, "Template", EObjectFlags::DefaultSubobject));
	ASSERT_TRUE(Template->IsTemplateObject());
	ASSERT_FALSE(Template->IsClassDefaultObject());
	Template->DTickableWorldSubsystem::Initialize(Collection);
	EXPECT_TRUE(Template->IsInitialized());
	EXPECT_EQ(Template->GetTickableTickType(), ETickableTickType::Never);
	int Ticks = 0;
	FSubsystemProbeA::Callback = [&](auto&, std::string_view Event) { if (Event == "Tick") ++Ticks; };
	FTickableGameObject::TickObjects(World, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(0, Ticks);
	Template->DTickableWorldSubsystem::Deinitialize();
	MarkObjectHierarchyAsGarbage(Template.Get());
}

TEST_F(FWorldSubsystemTests, TickLifetimeStartsAtInitializeAndEndsAtCleanup)
{
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	const auto* Default = Cast<DTickableWorldSubsystem>(FSubsystemProbeA::StaticClass()->GetDefaultObject());
	ASSERT_NE(Default, nullptr);
	EXPECT_FALSE(Default->IsInitialized());
	EXPECT_EQ(Default->GetTickableTickType(), ETickableTickType::Never);
	auto* World = MakeWorld();
	int Ticks = 0;
	FSubsystemProbeA::Callback = [&](auto& Object, std::string_view Event) {
		if (Event == "Tick") ++Ticks;
		if (Event == "Deinit") {
			EXPECT_FALSE(Object.IsInitialized());
			EXPECT_EQ(Object.GetTickableTickType(), ETickableTickType::Never);
		}
	};
	World->Tick({});
	EXPECT_EQ(Ticks, 0);
	World->InitializeSubsystems();
	TStrongObjectPtr<FSubsystemProbeA> Service(World->GetSubsystem<FSubsystemProbeA>());
	ASSERT_NE(Service.Get(), nullptr);
	EXPECT_TRUE(Service->IsInitialized());
	EXPECT_EQ(Service->GetTickableGameObjectWorld(), World);
	World->Tick({});
	EXPECT_EQ(Ticks, 1); // No Level or BeginPlay required.
	World->CleanupWorld();
	World->Tick({});
	EXPECT_EQ(Ticks, 1);
	EXPECT_EQ(Service->GetTickableTickType(), ETickableTickType::Never);
}

TEST_F(FWorldSubsystemTests, ExcludedRequiredDependencyIsConsumerContract)
{
	FSubsystemProbeA::Dependencies[FSubsystemProbeB::StaticClass()] = {FSubsystemProbeA::StaticClass()};
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
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

TEST_F(FWorldSubsystemTests, ExcludedServiceIsNotConstructedWhenEligibilityRequestsShutdown)
{
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld();
	FSubsystemProbeA::Create = [&](const auto&, DObject*) { World->BeginTearingDown(); return false; };
	EXPECT_NO_THROW(World->InitializeSubsystems());
	EXPECT_EQ(FSubsystemProbeA::InstanceConstructions, 0);
}

TEST_F(FWorldSubsystemTests, ShutdownRequestedBeforeInitializationDoesNotConstructServices)
{
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld();
	World->BeginTearingDown();
	EXPECT_NO_THROW(World->InitializeSubsystems());
	EXPECT_EQ(FSubsystemProbeA::InstanceConstructions, 0);
	EXPECT_FALSE(World->AreSubsystemsInitialized());
}

TEST_F(FWorldSubsystemTests, EligibilityExceptionFailsWithoutConstructingServices)
{
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	FSubsystemProbeA::Create = [](const auto&, DObject*) -> bool { throw std::runtime_error("eligibility"); };
	auto* World = MakeWorld();
	EXPECT_THROW(World->InitializeSubsystems(), std::exception);
	EXPECT_FALSE(World->AreSubsystemsInitialized());
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
	World->CleanupWorld();
	EXPECT_TRUE(Engine->GetWorldContexts().empty());
	Engine->PreExit();
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	EXPECT_FALSE(World->AreSubsystemsInitialized());
	EXPECT_EQ(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
	EXPECT_EQ(Cleanup, (std::vector<DClass*>{FSubsystemProbeB::StaticClass(), FSubsystemProbeA::StaticClass()}));


	World->CleanupWorld(); World->CleanupWorld();
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
	EXPECT_FALSE(Retained->IsInitialized());
	EXPECT_EQ(Retained->GetTickableTickType(), ETickableTickType::Never);
	EXPECT_EQ(1, Cleanups);
	CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_TRUE(IsValid(Retained.Get()));
	TWeakObjectPtr<FSubsystemProbeA> Weak(Retained.Get());
	Retained.Reset(); CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(Weak.IsValid());
	EXPECT_FALSE(World->AreSubsystemsInitialized());
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
	World->CleanupWorld();
	EXPECT_EQ(Events, (std::vector<std::string>{"Attach", "Begin", "End", "Begin", "End", "Detach", "Attach", "Detach", "Deinit"}));
}

TEST_F(FWorldSubsystemTests, BeginPlayRetirementCompletesBatchBeforeReverseCleanup)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
	std::vector<std::string> Events;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		Events.emplace_back(std::string(Object.GetClass() == FSubsystemProbeA::StaticClass() ? "A." : "B.") + std::string(Event));
		if (Event == "Begin") {
			if (Object.GetClass() == FSubsystemProbeA::StaticClass()) World->BeginTearingDown();
			EXPECT_TRUE(World->IsShutdownRequested());
			EXPECT_TRUE(World->AreSubsystemsInitialized());
			EXPECT_NE(World->GetCurrentLevel(), nullptr);
			EXPECT_FALSE(World->HasBegunPlay());
		}
	};
	EXPECT_EQ(World->BeginPlay({}).Error, EWorldPlayError::PlayAborted);
	EXPECT_FALSE(World->AreSubsystemsInitialized());
	EXPECT_EQ(Events, (std::vector<std::string>{"A.Begin", "B.Begin", "B.End", "A.End", "B.Detach", "A.Detach", "B.Deinit", "A.Deinit"}));
	World->CleanupWorld();
	EXPECT_EQ(Events.size(), 8u);
}

TEST_F(FWorldSubsystemTests, EndPlayRequestDuringBeginPairsWholeBatchAndAllowsAnotherPlay)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
	int Begins = 0, Ends = 0;
	bool bRequestEnd = true;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Begin") {
			++Begins;
			if (bRequestEnd && Object.GetClass() == FSubsystemProbeA::StaticClass()) World->EndPlay();
			EXPECT_FALSE(World->HasBegunPlay());
		}
		if (Event == "End") { ++Ends; World->EndPlay(); }
	};
	EXPECT_EQ(World->BeginPlay({}).Error, EWorldPlayError::PlayAborted);
	EXPECT_EQ(Begins, 2); EXPECT_EQ(Ends, 2);
	EXPECT_TRUE(World->AreSubsystemsInitialized());
	EXPECT_EQ(World->GetPlayState(), EWorldPlayState::Stopped);
	World->EndPlay(); EXPECT_EQ(Ends, 2);
	bRequestEnd = false;
	ASSERT_TRUE(World->BeginPlay({}));
	World->EndPlay();
	EXPECT_EQ(Begins, 4); EXPECT_EQ(Ends, 4);
}

TEST_F(FWorldSubsystemTests, BootstrapStopDoesNotEnterOrEndSubsystemPlayBatch)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "Level");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	ASSERT_NE(World->SpawnActor<APlayerStart>("Start"), nullptr);
	int Begins = 0, Ends = 0;
	FSubsystemProbeA::Callback = [&](auto&, std::string_view Event) {
		if (Event == "Begin") ++Begins;
		if (Event == "End") ++Ends;
	};
	EXPECT_EQ(World->BeginPlay({.GameModeClass = FStoppingBootstrapGameMode::StaticClass()}).Error, EWorldPlayError::PlayAborted);
	EXPECT_EQ(Begins, 0); EXPECT_EQ(Ends, 0);
	EXPECT_EQ(Level->GetActors().size(), 1u);
	EXPECT_EQ(World->GetPlayState(), EWorldPlayState::Stopped);
	ASSERT_TRUE(World->BeginPlay({.GameModeClass = AGameMode::StaticClass()}));
	World->EndPlay();
	EXPECT_EQ(Begins, 2); EXPECT_EQ(Ends, 2);
}

TEST_F(FWorldSubsystemTests, AttachmentRetirementCompletesBatchAndPairsEveryDetach)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	auto* Level = NewObject<DLevel>(World, "Level");
	std::vector<std::string> Events;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		Events.emplace_back(std::string(Object.GetClass() == FSubsystemProbeA::StaticClass() ? "A." : "B.") + std::string(Event));
		if (Event == "Attach") {
			if (Object.GetClass() == FSubsystemProbeA::StaticClass()) World->BeginTearingDown();
			EXPECT_EQ(Level->GetWorld(), World);
			EXPECT_EQ(World->GetCurrentLevel(), Level);
		}
		if (Event == "Detach") {
			EXPECT_EQ(Level->GetWorld(), World);
			World->BeginTearingDown();
		}
	};
	EXPECT_FALSE(World->SetCurrentLevel(Level));
	EXPECT_EQ(Events, (std::vector<std::string>{"A.Attach", "B.Attach", "B.Detach", "A.Detach", "B.Deinit", "A.Deinit"}));
	EXPECT_EQ(World->GetCurrentLevel(), nullptr);
	EXPECT_FALSE(World->AreSubsystemsInitialized());
}

TEST_F(FWorldSubsystemTests, TickableQueriesAreLiveAndEditorOptInIsPerInstance)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	for (auto Type : {EWorldType::Editor, EWorldType::EditorPreview}) {
		auto* World = MakeWorld(Type);
		World->InitializeSubsystems();
		int Ticks = 0;
		FSubsystemProbeA::Callback = [&](auto& Object, std::string_view Event) {
			if (Event != "Tick") return;
			++Ticks;
			if (Object.GetClass() == FSubsystemProbeA::StaticClass())
				World->GetSubsystem<FSubsystemProbeB>()->bTickable = false;
		};
		World->Tick({});
		EXPECT_EQ(Ticks, 0);
		World->GetSubsystem<FSubsystemProbeA>()->bTickInEditor = true;
		World->GetSubsystem<FSubsystemProbeB>()->bTickInEditor = true;
		World->Tick({});
		EXPECT_EQ(Ticks, 1); // A disables B before B's query in the same frame.
		World->GetSubsystem<FSubsystemProbeA>()->bTickable = false;
		World->GetSubsystem<FSubsystemProbeB>()->bTickable = true;
		World->Tick({});
		EXPECT_EQ(Ticks, 2);
		FSubsystemProbeA::Callback = {};
	}
}

TEST_F(FWorldSubsystemTests, PauseOptInSingleStepAndPlayLifetimeAreIndependent)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	for (auto Type : {EWorldType::Game, EWorldType::PIE}) {
		auto* World = MakeWorld(Type);
		World->InitializeSubsystems();
		auto* Service = World->GetSubsystem<FSubsystemProbeA>();
		int Ticks = 0;
		FSubsystemProbeA::Callback = [&](auto&, std::string_view Event) { if (Event == "Tick") ++Ticks; };
		ASSERT_TRUE(World->SetTimeScale(0.5f));
		World->Tick({.DeltaSeconds = 0.2f});
		EXPECT_EQ(Ticks, 2);
		EXPECT_FLOAT_EQ(Service->LastDelta, 0.1f);
		ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
		ASSERT_TRUE(World->BeginPlay({}));
		World->SetPaused(true);
		World->Tick({});
		EXPECT_EQ(Ticks, 2);
		Service->bTickWhenPaused = true;
		World->Tick({});
		EXPECT_EQ(Ticks, 3);
		World->RequestSingleStep();
		World->Tick({});
		EXPECT_EQ(Ticks, 5);
		World->Tick({});
		EXPECT_EQ(Ticks, 6);
		World->SetPaused(false);
		World->EndPlay();
		World->Tick({});
		EXPECT_EQ(Ticks, 8); // EndPlay does not deinitialize tickable services.
		FSubsystemProbeA::Callback = {};
	}
}

TEST_F(FWorldSubsystemTests, TickRunsAfterTimersAndHonorsTheirRetirementRequests)
{
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld();
	World->InitializeSubsystems();
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
	ASSERT_TRUE(World->BeginPlay({}));
	std::vector<std::string> Events;
	FSubsystemProbeA::Callback = [&](auto&, std::string_view Event) {
		if (Event == "Tick") Events.emplace_back(Event);
	};
	World->GetTimerManager().SetTimerForNextTick([&] { Events.emplace_back("Timer"); });
	World->Tick({});
	EXPECT_EQ(Events, (std::vector<std::string>{"Timer", "Tick"}));
	World->GetTimerManager().SetTimerForNextTick([&] {
		Events.emplace_back("Stop");
		World->BeginTearingDown();
	});
	World->Tick({});
	EXPECT_EQ(Events, (std::vector<std::string>{"Timer", "Tick", "Stop"}));
	EXPECT_FALSE(World->AreSubsystemsInitialized());
}

TEST_F(FWorldSubsystemTests, CollisionStateIsIsolatedAndClearedAcrossDetachShutdownAndDuplication)
{
	auto* Editor = MakeWorld(EWorldType::Editor);
	auto* PIE = MakeWorld(EWorldType::PIE);
	auto* Preview = MakeWorld(EWorldType::EditorPreview);
	for (auto* World : {Editor, PIE, Preview}) ASSERT_NO_THROW(World->InitializeSubsystems());
	EXPECT_EQ(Cast<DTickableWorldSubsystem>(Editor->GetSubsystem<DCollisionDebugSubsystem>()), nullptr);
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
	PIE->CleanupWorld(); Preview->CleanupWorld();
	EXPECT_TRUE(Editor->IsCollisionDebugDrawEnabled());
	Editor->CleanupWorld(); EXPECT_FALSE(Editor->IsCollisionDebugDrawEnabled());
}

TEST_F(FWorldSubsystemTests, ProviderRequiresFrozenWorldShutdownAndOwnsRegistrationCleanup)
{
	class FProvider : public IModuleInterface
	{
	public:
		auto SupportsDynamicReloading() const -> bool override { return true; }
		auto StartupModule() -> void override
		{
			Registration = std::make_unique<FWorldSubsystemRegistration>(FSubsystemDescriptor{
				.Type = FSubsystemProbeA::StaticClass(), .Provider = FModuleStartup::GetModuleName()});
		}
		auto CanShutdownModule() const -> bool override
		{ return !World || (World->IsShutdownRequested() && World->IsReadyForFinishDestroy() && !World->AreSubsystemsInitialized()); }
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
	FSubsystemProbeA::Callback = [&](auto&, std::string_view Event) {
		if (Event == "Deinit") EXPECT_FALSE(Manager.UnloadModule("WorldSubsystemFixtureProvider"));
	};
	World->CleanupWorld();
	FSubsystemProbeA::Callback = {};
	EXPECT_TRUE(Cancellation->IsCanceled());
	// Probe classes live in the test executable, so this fixture can release its logical module.
	EXPECT_TRUE(Manager.UnloadModule("WorldSubsystemFixtureProvider"));
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	World->CleanupWorld();
	{ std::lock_guard Lock(Payload->Mutex); Payload->bReleased = true; }
	Payload->Condition.notify_all();
	EXPECT_EQ(WaitTask(Worker).WaitStatus, ETaskWaitStatus::Completed);
	int Published = 0;
	// This is the owner's GameThread completion boundary; no retired object is resolved.
	if (!Cancellation->IsCanceled()) Published = Payload->Value;
	EXPECT_EQ(Published, 0);
	EXPECT_TRUE(Cancellation->IsCanceled());
}

TEST_F(FWorldSubsystemTests, InitializationRequestsCollectionWithoutDestroyingUnpublishedWorld)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FSubsystemProbeA::Callback = [](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Init") { RequestGarbageCollection(); EXPECT_TRUE(IsGarbageCollectionRequested()); EXPECT_FALSE(Object.IsPendingKill()); EXPECT_NE(Object.GetWorld(), nullptr); }
	};
	auto* World = NewObject<DWorld>(nullptr, "UnpublishedWorld");
	ASSERT_NO_THROW(World->InitializeSubsystems());
	Worlds.emplace_back(World);
	EXPECT_NE(World->GetSubsystem<FSubsystemProbeA>(), nullptr);
}

TEST_F(FWorldSubsystemTests, TickEndPlayCompletesBatchAndRetainsServicesForAnotherPlay)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
	ASSERT_TRUE(World->BeginPlay({}));
	int Ticks = 0;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Object, std::string_view Event) {
		if (Event == "Tick") {
			++Ticks;
			World->EndPlay(); EXPECT_TRUE(World->HasBegunPlay());
		}
	};
	World->Tick({});
	EXPECT_EQ(Ticks, 2); EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_TRUE(World->AreSubsystemsInitialized());
	ASSERT_TRUE(World->BeginPlay({}));
}

TEST_F(FWorldSubsystemTests, AttachmentTransitionCompletesBatchAndAppliesAtNextTick)
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
			RequestGarbageCollection();
			EXPECT_TRUE(IsGarbageCollectionRequested());
		}
	};
	ASSERT_TRUE(World->SetCurrentLevel(First));
	EXPECT_EQ(AttachCount, 2);
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Second);
	EXPECT_EQ(AttachCount, 4);
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

TEST_F(FWorldSubsystemTests, LevelSwitchRetainsBothLevelsAcrossCallbackCollectionRequests)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld(); ASSERT_NO_THROW(World->InitializeSubsystems());
	auto* First = NewObject<DLevel>(World, "First");
	ASSERT_TRUE(World->SetCurrentLevel(First));
	auto* Second = NewObject<DLevel>(World, "Second");
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA&, std::string_view Event) {
		if (Event == "Attach" || Event == "Detach") {
			RequestGarbageCollection();
			EXPECT_TRUE(IsGarbageCollectionRequested());
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

		RequestGarbageCollection();
		EXPECT_TRUE(IsGarbageCollectionRequested());
		EXPECT_FALSE(Second->IsPendingKill());
		EXPECT_FALSE(World->SetCurrentLevel(Third));
		EXPECT_TRUE(World->RequestLevelTransition(Third));
	};
	ASSERT_TRUE(World->RequestLevelTransition(Second));
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Second);
	EXPECT_FALSE(Second->IsPendingKill());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(Third->IsPendingKill());
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Third);
}

TEST_F(FWorldSubsystemTests, TickRequestsCollectionAndRejectsRecursiveWorldOperations)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
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
		RequestGarbageCollection();
		EXPECT_TRUE(IsGarbageCollectionRequested());
		EXPECT_NE(Garbage.Get(), nullptr);
	};
	World->Tick({});
	EXPECT_EQ(Ticks, 1);
	EXPECT_FALSE(World->HasBegunPlay());

	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
		RequestGarbageCollection();
		EXPECT_TRUE(IsGarbageCollectionRequested());
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
			Object.GetWorld()->BeginTearingDown();
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
		auto Dispatch(const std::function<void()>& Callback) -> void { Callback(); }
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
			FEngineSubsystemProbe::StaticClass(), FEditorSubsystemProbe::StaticClass(), FStoppingBootstrapGameMode::StaticClass()};
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(EngineObject.IsValid()); EXPECT_FALSE(EditorObject.IsValid()); EXPECT_FALSE(WorldObject.IsValid());
	EXPECT_EQ(0, Cleanups);
	EXPECT_TRUE(Host->AreSubsystemsInitialized());
	EXPECT_TRUE(Host->AreEditorSubsystemsInitialized());
	EXPECT_TRUE(World->AreSubsystemsInitialized());

	World->CleanupWorld(); Host->PreExit();
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
	CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_TRUE(WeakEngine.IsValid()); EXPECT_TRUE(WeakWorld.IsValid());
	EngineService.Reset(); WorldService.Reset(); CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(WeakEngine.IsValid()); EXPECT_FALSE(WeakWorld.IsValid());
	EXPECT_TRUE(Host->AreSubsystemsInitialized());
	EXPECT_TRUE(World->AreSubsystemsInitialized());
	Host->PreExit(); MarkObjectHierarchyAsGarbage(Host.Get());
}

TEST_F(FWorldSubsystemTests, PendingHostShutdownDoesNotStartEngineServices)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	int Constructions = 0;
	FEngineSubsystemProbe::ConstructorCallback = [&](auto&) { ++Constructions; };
	Host->DispatchSubsystemCallback([&] { Host->RequestShutdown(); });
	EXPECT_NO_THROW(Host->StartEngine());
	FEngineSubsystemProbe::ConstructorCallback = {};
	EXPECT_EQ(Constructions, 0);
	EXPECT_FALSE(Host->AreSubsystemsInitialized());
	MarkObjectHierarchyAsGarbage(Host.Get());
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
	Host->PreExit();
	EXPECT_EQ(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	Host->PreExit();
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
	EXPECT_EQ(Host->GetPlayState(), EPlayState::Stopped);
	EXPECT_EQ(Host->GetPlayWorld(), nullptr);
	EXPECT_EQ(Host->GetWorld(), World);
	EXPECT_EQ(Host->GetWorldContexts().size(), 1u);
	FSubsystemProbeA::Callback = {};
	ASSERT_TRUE(Host->StartPlay(World->GetCurrentLevel()));
	Host->StopPlaySession();
	Host->PreExit();
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(First->GetSubsystem<FEngineSubsystemProbe>(), EngineService);
	EXPECT_EQ(First->GetEditorSubsystem<FEditorSubsystemProbe>(), EditorService);
	PreviewWorld->CleanupWorld();
	EXPECT_EQ(First->GetWorldContextFromWorld(PreviewWorld), nullptr);
	First->PreExit(); First->PreExit();
	EXPECT_EQ(First->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	ASSERT_NO_THROW(Second->StartEngine());
	ASSERT_NO_THROW(Second->StartEditor()); EXPECT_EQ(GEditor, Second.Get());
	Second->PreExit();
}

TEST_F(FWorldSubsystemTests, HostShutdownDuringInitializationAndDispatchIsDeferred)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	int Cleanups = 0;
	FEngineSubsystemProbe::Callback = [&](FEngineSubsystemProbe& Service, bool bInit) {
		if (bInit) {
			Service.GetEngine()->RequestShutdown();
			EXPECT_TRUE(Service.GetEngine()->IsShutdownRequested());

			EXPECT_EQ(Cleanups, 0);
		} else ++Cleanups;
	};
	EXPECT_NO_THROW(Host->StartEngine());
	EXPECT_EQ(Cleanups, 0);
	Host->PreExit();
	EXPECT_EQ(Cleanups, 1);
	FEngineSubsystemProbe::Callback = {};
	TStrongObjectPtr<FSubsystemEditorHost> Ready(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Ready->StartEngine());

	Ready->Dispatch([&] {
		Ready->RequestShutdown();
		EXPECT_TRUE(Ready->IsShutdownRequested());
		EXPECT_NE(Ready->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
		RequestGarbageCollection();
	});
	EXPECT_NE(Ready->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
	Ready->PreExit();
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
		Host->RequestShutdown();
		EXPECT_EQ(1, Host->ShutdownRequests);
	};
	Host->OnShutdownRequested = [&] {
		Host->RequestShutdown();
		EXPECT_EQ(1, Host->ShutdownRequests);
		EXPECT_NE(nullptr, Host->GetSubsystem<FEngineSubsystemProbe>());
		EXPECT_EQ(0, Cleanups);
	};
	Host->Dispatch([&] {
		Host->RequestShutdown(); Host->RequestShutdown();
		Host->DispatchSubsystemCallback([&] { ++ExternalCalls; });
		EXPECT_EQ(1, Host->ShutdownRequests);
		EXPECT_EQ(0, Cleanups); EXPECT_EQ(0, ExternalCalls);
	});
	EXPECT_EQ(0, Cleanups);
	Host->PreExit();
	EXPECT_EQ(1, Cleanups);
	EXPECT_FALSE(Host->AreSubsystemsInitialized());
	Host->PreExit();
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
	Host->PreExit();
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
	Host->PreExit();
}

TEST_F(FWorldSubsystemTests, EditorInitializationCanRequestShutdownOrThrow)
{
	FEditorSubsystemRegistration Token({.Type = FEditorSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	int Cleanups = 0;
	FEditorSubsystemProbe::Callback = [&](FEditorSubsystemProbe& Service, bool bInit) {
		if (bInit) {
			Service.GetEditor()->RequestShutdown();
			EXPECT_TRUE(Service.GetEditor()->IsShutdownRequested());

			EXPECT_EQ(Cleanups, 0);
		} else ++Cleanups;
	};
	EXPECT_NO_THROW(Host->StartEditor());
	EXPECT_EQ(Cleanups, 0);
	Host->PreExit();
	EXPECT_EQ(Cleanups, 1);
	FEditorSubsystemProbe::Callback = {};
	TStrongObjectPtr<FSubsystemEditorHost> Failed(NewObject<FSubsystemEditorHost>(nullptr, {}));
	FEditorSubsystemProbe::Callback = [](FEditorSubsystemProbe&, bool bInit) { if (bInit) throw std::runtime_error("fixture"); };
	EXPECT_THROW(Failed->StartEditor(), std::exception);
	EXPECT_EQ(Failed->GetEditorSubsystem<FEditorSubsystemProbe>(), nullptr);
	FEditorSubsystemProbe::Callback = {};
	Failed->PreExit();
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
	Host->GetNotificationManager().Post({.Message = "Ready", .Action = FNotificationAction{
		.Label = "Open", .Invoke = [&] { ++Invocations; }}});
	Host->GetNotificationManager().Tick(0.0f);
	ASSERT_EQ(Service->GetManager().GetNotifications().size(), 1u);
	auto Action = Service->GetManager().GetNotifications().front().Action;

	Host->PreExit();

	EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), nullptr);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	Action->Invoke();
	EXPECT_FALSE(Action->IsEnabled());
	EXPECT_EQ(Invocations, 0);
}

TEST_F(FWorldSubsystemTests, EngineRetirementCompletesAdmittedWorldTickBatch)
{
	FEngineSubsystemRegistration EngineToken({.Type = FEngineSubsystemProbe::StaticClass()});
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	ASSERT_NO_THROW(Host->StartEngine());
	auto* World = MakeWorld(EWorldType::EditorPreview);
	ASSERT_NO_THROW(World->InitializeSubsystems());
	Host->SetWorld(World);
	World->GetSubsystem<FSubsystemProbeA>()->bTickInEditor = true;
	World->GetSubsystem<FSubsystemProbeB>()->bTickInEditor = true;
	int Ticks = 0;
	bool bInsideCallback = false;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Service, std::string_view Event) {
		if (Event == "Tick") {
			++Ticks;
			bInsideCallback = true;
			Host->RequestShutdown();
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
	EXPECT_EQ(Ticks, 2);
	EXPECT_FALSE(World->AreSubsystemsInitialized());
	Host->PreExit();
	EXPECT_EQ(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
}

TEST_F(FWorldSubsystemTests, ConstructorShutdownRequestFinishesInitializeBeforeHostCleanup)
{
	FEngineSubsystemRegistration Token({.Type = FEngineSubsystemProbe::StaticClass()});
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	int Initializations = 0;
	int Cleanups = 0;
	FEngineSubsystemProbe::ConstructorCallback = [&](FEngineSubsystemProbe&) { Host->RequestShutdown(); };
	FEngineSubsystemProbe::Callback = [&](FEngineSubsystemProbe& Service, bool bInit) {
		if (bInit) { ++Initializations; EXPECT_TRUE(Service.GetEngine()->IsShutdownRequested()); EXPECT_EQ(Cleanups, 0); }
		else { ++Cleanups; }
	};
	EXPECT_NO_THROW(Host->StartEngine());
	EXPECT_EQ(Initializations, 1);
	EXPECT_EQ(Cleanups, 0);
	Host->PreExit();
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
	const auto Id = Host->GetNotificationManager().Post({.Message = "Stop", .Action = FNotificationAction{
		.Label = "Stop", .Invoke = [&] {
			Host->RequestShutdown();
			EXPECT_FALSE(Service->GetManager().IsAcceptingRequests());
			EXPECT_EQ(0u, Service->GetManager().Post({.Message = "Too late"}));
			EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), Service);
			EXPECT_EQ(Service->GetManager().GetNotifications().size(), 1u);
			RequestGarbageCollection();
			EXPECT_FALSE(Service->IsPendingKill());
		}}});
	Host->GetNotificationManager().Tick(0.0f);
	EXPECT_TRUE(Host->GetNotificationManager().InvokeAction(Id));
	EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), Service);
	Host->Tick(0.0f, false);
	EXPECT_EQ(Host->GetEditorSubsystem<DEditorNotificationSubsystem>(), Service);
	Host->PreExit();
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	std::vector<DClass*> Cleaned;
	FSubsystemProbeA::Callback = [&](FSubsystemProbeA& Service, std::string_view Event) {
		if (Event == "Deinit") { EXPECT_EQ(Initializations, 2); Cleaned.push_back(Service.GetClass()); }
		if (Event != "Init") return;
		++Initializations;
		EXPECT_EQ(Host->GetWorld(), nullptr);
		Host->RequestShutdown();
		EXPECT_TRUE(Service.GetWorld()->IsShutdownRequested());
		EXPECT_NE(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
		Host->DispatchSubsystemCallback([] { ADD_FAILURE() << "Retired hosts must reject external work during initialization."; });
	};
	EXPECT_NO_THROW(World->InitializeSubsystems());
	EXPECT_EQ(Initializations, 2);
	EXPECT_EQ(Cleaned, (std::vector<DClass*>{FSubsystemProbeB::StaticClass(), FSubsystemProbeA::StaticClass()}));
	EXPECT_EQ(Host->GetWorld(), nullptr);
	Host->PreExit();
	EXPECT_EQ(Host->GetSubsystem<FEngineSubsystemProbe>(), nullptr);
}

TEST_F(FWorldSubsystemTests, ActorEditingAdmitsOnlyItsLiveHostAndEditableStoppedDocument)
{
	using namespace Durin;
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
	First->PreExit();
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
		Host->RequestShutdown();
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
	Host->PreExit();
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
	World->BeginTearingDown();
	EXPECT_EQ(World->GetCurrentLevel(), Level);
	EXPECT_TRUE(World->IsShutdownRequested());
	EXPECT_FALSE(Service->SpawnActor(*Level, AActor::StaticClass(), "Closing", {}, {false}));
	EXPECT_FALSE(Host->GetTransactor()->Undo());
	EXPECT_EQ(Host->GetTransactor()->GetUndoId(), UndoId);
	Host->PreExit();
}


TEST_F(FWorldSubsystemTests, EngineDispatchesWorldlessBatchBeforeApplyingShutdown)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	Host->StartEngine();
	auto* World = MakeWorld();
	World->InitializeSubsystems();
	Host->SetWorld(World);
	std::vector<std::string> Events;
	FTickableProbe WorldObject(World), Global, Later;
	WorldObject.Callback = [&] { Events.emplace_back("World"); };
	TWeakObjectPtr<DObject> Garbage(NewObject<DObject>(nullptr, "WorldlessCallbackGarbage"));
	Global.Callback = [&] { Events.emplace_back("Global"); Host->ForceGarbageCollection(); Host->RequestShutdown(); };
	Later.Callback = [&] { Events.emplace_back("Later"); };
	Host->Tick(0.25f, false);
	EXPECT_EQ(Events, (std::vector<std::string>{"World", "Global", "Later"}));
	EXPECT_EQ(Later.Ticks, 1);
	EXPECT_TRUE(Garbage.IsValid());
	EXPECT_TRUE(IsGarbageCollectionRequested());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(Garbage.IsValid());
	Host->Tick(0.25f, false);
	EXPECT_EQ(Global.Ticks, 1);
	Host->PreExit();
}


TEST_F(FWorldSubsystemTests, EditorWorldlessTickRequiresAWorldDispatch)
{
	TStrongObjectPtr<FSubsystemEditorHost> Host(NewObject<FSubsystemEditorHost>(nullptr, {}));
	Host->StartEngine();
	FTickableProbe Object;
	Host->Tick(0.25f, false);
	EXPECT_EQ(Object.Ticks, 0);
	Object.bEditor = true;
	Host->Tick(0.25f, false);
	EXPECT_EQ(Object.Ticks, 0);
	auto* World = MakeWorld(EWorldType::Editor);
	World->InitializeSubsystems();
	Host->SetEditorWorld(World);
	Host->Tick(0.25f, false);
	EXPECT_EQ(Object.Ticks, 1);
	EXPECT_FLOAT_EQ(Object.Delta, 0.25f);
	Host->PreExit();
}


TEST_F(FWorldSubsystemTests, GameEngineDispatchesWorldlessObjectsWithoutAWorld)
{
	GIsEditor = false;
	TStrongObjectPtr<DGameEngine> Host(NewObject<DGameEngine>(nullptr, {}));
	FTickableProbe Object;
	Host->Tick(0.25f, false);
	EXPECT_EQ(Object.Ticks, 1);
	Host->Tick(-1.0f, false);
	Host->Tick(std::numeric_limits<float>::quiet_NaN(), false);
	EXPECT_EQ(Object.Ticks, 1);
	Host->PreExit();
}

TEST_F(FWorldSubsystemTests, ProcessExitClosesNextTickBatchWithoutInterruptingCurrentBatch)
{
	FTickableProbe First, Later;
	const bool bPreviousExit = IsEngineExitRequested();
	First.Callback = [] { RequestEngineExit(); };
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Later.Ticks, 1);
	FTickableGameObject::TickObjects(nullptr, LEVELTICK_All, false, 0.0f);
	EXPECT_EQ(Later.Ticks, 1);
	GIsRequestingExit = bPreviousExit;
}

TEST_F(FWorldSubsystemTests, TickableLifecycleDiagnosesMissingSuperAndUnpairedDestruction)
{
	FWorldSubsystemRegistration Token({.Type = FSubsystemProbeA::StaticClass()});
	auto* World = MakeWorld();
	FSubsystemProbeA::bFailBeforeSuper = true;
	EXPECT_DEATH(World->InitializeSubsystems(), "Super::Initialize first");
	FSubsystemProbeA::bFailBeforeSuper = false;
	World->InitializeSubsystems();
	auto* Object = World->GetSubsystem<FSubsystemProbeA>();
	EXPECT_DEATH(Object->BeginDestroy(), "before Super::Deinitialize");
	FSubsystemCollection Unused(*World, DWorldSubsystem::StaticClass());
	EXPECT_DEATH(Object->Initialize(Unused), "must be paired");
	World->CleanupWorld();
	EXPECT_FALSE(Object->IsInitialized());
	EXPECT_EQ(Object->GetTickableTickType(), ETickableTickType::Never);
	EXPECT_DEATH(Object->Deinitialize(), "Super::Initialize first");
	EXPECT_DEATH(Object->IsAllowedToTick(), "registered before initialization");
}

TEST_F(FWorldSubsystemTests, TickableTransitionRequestCompletesBatchBeforeNextWorldFrame)
{
	FWorldSubsystemRegistration A({.Type = FSubsystemProbeA::StaticClass()});
	FWorldSubsystemRegistration B({.Type = FSubsystemProbeB::StaticClass()});
	auto* World = MakeWorld();
	World->InitializeSubsystems();
	auto* First = NewObject<DLevel>(World, "First");
	auto* Next = NewObject<DLevel>(World, "Next");
	ASSERT_TRUE(World->SetCurrentLevel(First));
	int Ticks = 0;
	FSubsystemProbeA::Callback = [&](auto& Object, std::string_view Event) {
		if (Event != "Tick") return;
		++Ticks;
		EXPECT_EQ(World->GetCurrentLevel(), First);
		if (Object.GetClass() == FSubsystemProbeA::StaticClass())
			EXPECT_TRUE(World->RequestLevelTransition(Next));
	};
	World->Tick({});
	EXPECT_EQ(Ticks, 2);
	EXPECT_EQ(World->GetCurrentLevel(), First);
	FSubsystemProbeA::Callback = {};
	World->Tick({});
	EXPECT_EQ(World->GetCurrentLevel(), Next);
}
