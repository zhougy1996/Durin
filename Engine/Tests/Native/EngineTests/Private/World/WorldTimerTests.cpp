#include "WorldTestSupport.h"
#include "DObject/DObjectArray.h"
#include <stdexcept>

namespace Durin
{
	// Captures the public gameplay delta and schedules work from a real PostPhysics callback.
	class FTimerTestActor : public AActor
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FTimerTestActor, AActor, FTimerTestActor::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FTimerTestActor)
	public:
		explicit FTimerTestActor(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer)
		{
			SetActorTickEnabled(true);
			GetPrimaryActorTick().SetTickGroup(ETickingGroup::PostPhysics);
		}
		auto Tick(float DeltaSeconds) -> void override { if (Callback) Callback(DeltaSeconds); }
		auto BeginPlay() -> void override { Super::BeginPlay(); if (OnBegin) OnBegin(); }
		std::function<void(float)> Callback;
		std::function<void()> OnBegin;
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FTimerTestActor)
}

namespace
{
	class FWorldTimerTests : public testing::Test
	{
	protected:
		Durin::DWorld* World = nullptr;
		auto SetUp() -> void override
		{
			World = CreateWorld();
			ASSERT_TRUE(World->BeginPlay({}));
		}
		auto TearDown() -> void override
		{
			World->Shutdown();
			Durin::MarkObjectHierarchyAsGarbage(World);
			Durin::CollectGarbage();
		}
		auto Tick(float Delta = 0.0f) -> void { World->Tick({.DeltaSeconds = Delta}); }
	};
}

TEST_F(FWorldTimerTests, UniqueCapturesSurviveRepeatsAndReleaseOnClear)
{
	auto& Timers = World->GetTimerManager();
	int Calls = 0, Destroyed = 0, NextCalls = 0, ObjectCalls = 0;
	auto Delete = [&Destroyed](int* Value) { delete Value; ++Destroyed; };
	auto Repeating = Timers.SetTimer(0.0,
		[Value = std::unique_ptr<int, decltype(Delete)>(new int(0), Delete), &Calls]() {
			Calls = ++*Value;
		}, 1.0);
	Timers.SetTimerForNextTick([Value = std::make_unique<int>(7), &NextCalls] { NextCalls += *Value; });
	Timers.SetTimerForObject(World, 0.0,
		[Value = std::make_unique<int>(3), &ObjectCalls](Durin::DObject&) { ObjectCalls += *Value; });
	Timers.SetTimerForObjectNextTick(World,
		[Value = std::make_unique<int>(5), &ObjectCalls](Durin::DObject&) { ObjectCalls += *Value; });
	Tick();
	EXPECT_EQ(1, Calls);
	EXPECT_EQ(7, NextCalls);
	EXPECT_EQ(8, ObjectCalls);
	EXPECT_EQ(0, Destroyed);
	Tick(1.0f);
	EXPECT_EQ(2, Calls);
	EXPECT_TRUE(Timers.ClearTimer(Repeating));
	EXPECT_EQ(1, Destroyed);
	Tick(1.0f);
	EXPECT_EQ(2, Calls);
}

TEST_F(FWorldTimerTests, DelayLoopAndNextFrameSharePauseStepAndScale)
{
	auto& Timers = World->GetTimerManager();
	int Delayed = 0, Looped = 0, Next = 0;
	Timers.SetTimer(1.0, [&] { ++Delayed; });
	Timers.SetTimer(1.0, [&] { ++Looped; }, 1.0);
	Timers.SetTimerForNextTick([&] { ++Next; });
	World->SetPaused(true);
	Tick(10.0f);
	EXPECT_EQ(Next, 0);
	EXPECT_EQ(Timers.GetGameTimeSeconds(), 0.0);
	ASSERT_TRUE(World->SetTimeScale(0.5f));
	World->RequestSingleStep();
	Tick(1.0f);
	EXPECT_EQ(Next, 1);
	EXPECT_EQ(Delayed, 0);
	EXPECT_EQ(Timers.GetGameTimeSeconds(), 0.5);
	Tick(1.0f);
	EXPECT_EQ(Timers.GetGameTimeSeconds(), 0.5);
	World->SetPaused(false);
	Tick(1.0f);
	EXPECT_EQ(Delayed, 1);
	EXPECT_EQ(Looped, 1);
	Tick(9.0f);
	EXPECT_EQ(Looped, 2);
	Tick(1.0f);
	EXPECT_EQ(Looped, 3); // Original deadlines are 1, 2, ...; the long frame ended at 5.5.
}

TEST_F(FWorldTimerTests, ZeroScaleStillAdmitsNextFrameAndRejectsInvalidTime)
{
	auto& Timers = World->GetTimerManager();
	int Calls = 0;
	Timers.SetTimerForNextTick([&] { ++Calls; });
	Timers.SetTimer(1.0, [&] { Calls += 100; });
	ASSERT_TRUE(World->SetTimeScale(0.0f));
	EXPECT_FALSE(World->SetTimeScale(-1.0f));
	EXPECT_FALSE(World->SetTimeScale(std::numeric_limits<float>::infinity()));
	Tick(-1.0f);
	Tick(std::numeric_limits<float>::quiet_NaN());
	EXPECT_EQ(Calls, 0);
	Tick(100.0f);
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(Timers.GetGameTimeSeconds(), 0.0);
	EXPECT_FALSE(Timers.IsTimerActive(Timers.SetTimer(-1.0, [] {})));
	EXPECT_FALSE(Timers.IsTimerActive(Timers.SetTimer(0.0, [] {}, -1.0)));
	EXPECT_FALSE(Timers.IsTimerActive(Timers.SetTimer(std::numeric_limits<double>::infinity(), [] {})));
	EXPECT_FALSE(Timers.IsTimerActive(Timers.SetTimer(0.0, {})));
}

TEST_F(FWorldTimerTests, PostPhysicsUsesSnapshotDeltaAndDefersNewTimers)
{
	auto* Actor = World->SpawnActor<Durin::FTimerTestActor>();
	ASSERT_NE(Actor, nullptr);
	auto& Timers = World->GetTimerManager();
	std::vector<int> Events;
	Actor->Callback = [&](float Delta)
	{
		EXPECT_EQ(Delta, 0.5f);
		Events.push_back(1);
		World->SetTimeScale(3.0f);
		Timers.SetTimerForNextTick([&] { Events.push_back(3); });
		Timers.SetTimer(0.0, [&] { Events.push_back(4); });
	};
	World->SetTimeScale(0.5f);
	Timers.SetTimer(0.5, [&] { Events.push_back(2); });
	Tick(1.0f);
	EXPECT_EQ(Events, (std::vector<int>{1, 2}));
	EXPECT_EQ(Timers.GetGameTimeSeconds(), 0.5);
	Actor->Callback = {};
	Tick(1.0f);
	EXPECT_EQ(Events, (std::vector<int>{1, 2, 3, 4}));
	EXPECT_EQ(Timers.GetGameTimeSeconds(), 3.5);
}

TEST_F(FWorldTimerTests, MutationCancelsReadyEntriesAndDoesNotReviveReusedSlots)
{
	auto& Timers = World->GetTimerManager();
	std::vector<int> Events;
	Durin::FTimerHandle First, Second, Replacement;
	First = Timers.SetTimer(0.0, [&]
	{
		Events.push_back(1);
		EXPECT_TRUE(Timers.ClearTimer(First));
		EXPECT_TRUE(Timers.ClearTimer(Second));
		Replacement = Timers.SetTimer(0.0, [&] { Events.push_back(3); });
		EXPECT_FALSE(Timers.ClearTimer(Second));
		// Grow backing storage while the current callback remains on the stack.
		for (int Index = 0; Index < 256; ++Index) Timers.SetTimer(100.0, [] {});
	}, 1.0);
	Second = Timers.SetTimer(0.0, [&] { Events.push_back(2); });
	Tick();
	EXPECT_EQ(Events, (std::vector<int>{1}));
	EXPECT_FALSE(Timers.IsTimerActive(First));
	EXPECT_TRUE(Timers.IsTimerActive(Replacement));
	Tick();
	EXPECT_EQ(Events, (std::vector<int>{1, 3}));
}

TEST_F(FWorldTimerTests, PauseResumePreservesRemainingAndSelfPauseUsesNextPeriod)
{
	auto& Timers = World->GetTimerManager();
	int Calls = 0;
	Durin::FTimerHandle Handle;
	Handle = Timers.SetTimer(2.0, [&] { ++Calls; Timers.PauseTimer(Handle); }, 2.0);
	Tick(0.5f);
	EXPECT_TRUE(Timers.PauseTimer(Handle));
	EXPECT_EQ(Timers.GetTimerRemaining(Handle), 1.5);
	Tick(10.0f);
	EXPECT_EQ(Timers.GetTimerRemaining(Handle), 1.5);
	EXPECT_TRUE(Timers.UnpauseTimer(Handle));
	Tick(1.5f);
	EXPECT_EQ(Calls, 1);
	EXPECT_TRUE(Timers.IsTimerPaused(Handle));
	EXPECT_EQ(Timers.GetTimerRemaining(Handle), 2.0);
	Timers.UnpauseTimer(Handle);
	Tick(1.0f);
	EXPECT_EQ(Calls, 1);
	Tick(1.0f);
	EXPECT_EQ(Calls, 2);
}

TEST_F(FWorldTimerTests, ObjectDestructionCancelsBoundWorkAndReleasesCaptures)
{
	auto& Timers = World->GetTimerManager();
	auto* Actor = World->SpawnActor<Durin::AActor>();
	auto* Component = Actor->AddInstanceComponent(Durin::DActorComponent::StaticClass(), "TimerOwner");
	ASSERT_NE(Component, nullptr);
	int Calls = 0;
	auto Capture = std::make_shared<int>(1);
	std::weak_ptr<int> Weak = Capture;
	Timers.SetTimerForNextTick([&] { World->DestroyActor(Actor); });
	const auto A = Timers.SetTimerForObject(Actor, 0.0, [&, Capture](Durin::DObject&) { ++Calls; });
	const auto C = Timers.SetTimerForObject(Component, 0.0, [&](Durin::DObject&) { ++Calls; });
	Capture.reset();
	Tick();
	EXPECT_EQ(Calls, 0);
	EXPECT_FALSE(Timers.IsTimerActive(A));
	EXPECT_FALSE(Timers.IsTimerActive(C));
	EXPECT_TRUE(Weak.expired());
}

TEST_F(FWorldTimerTests, UnregisterAndActorEndPlayCancelWithoutPhysicalDestruction)
{
	auto& Timers = World->GetTimerManager();
	auto* Actor = World->SpawnActor<Durin::AActor>();
	auto* Component = Actor->AddInstanceComponent(Durin::DActorComponent::StaticClass(), "TimerOwner");
	int Calls = 0;
	const auto C = Timers.SetTimerForObjectNextTick(Component, [&](Durin::DObject&) { ++Calls; });
	Component->UnregisterComponent();
	Component->RegisterComponent();
	EXPECT_FALSE(Timers.IsTimerActive(C));
	const auto A = Timers.SetTimerForObjectNextTick(Actor, [&](Durin::DObject&) { ++Calls; });
	Actor->RouteEndPlay();
	Actor->DispatchBeginPlay();
	Tick();
	EXPECT_FALSE(Timers.IsTimerActive(A));
	EXPECT_EQ(Calls, 0);
}

TEST_F(FWorldTimerTests, GarbageMarkedTargetsAreSkippedAndForeignWorldHandlesRejected)
{
	auto& Timers = World->GetTimerManager();
	auto* Other = CreateWorld();
	ASSERT_TRUE(Other->BeginPlay({}));
	auto* Actor = World->SpawnActor<Durin::AActor>();
	int Calls = 0;
	const auto Handle = Timers.SetTimerForObjectNextTick(Actor, [&](Durin::DObject&) { ++Calls; });
	EXPECT_FALSE(Other->GetTimerManager().ClearTimer(Handle));
	EXPECT_EQ(Other->GetTimerManager().SetTimerForObjectNextTick(Actor, [](Durin::DObject&) {}).Generation, 0u);
	Durin::MarkAsGarbage(Actor);
	Tick();
	EXPECT_EQ(Calls, 0);
	EXPECT_FALSE(Timers.IsTimerActive(Handle));
	Other->Shutdown();
	Durin::MarkObjectHierarchyAsGarbage(Other);
}

TEST_F(FWorldTimerTests, EndPlayCallbackStopsBatchAndNewPlayInvalidatesOldHandles)
{
	auto& Timers = World->GetTimerManager();
	int Calls = 0;
	Timers.SetTimerForNextTick([&] { World->EndPlay(); });
	const auto Old = Timers.SetTimerForNextTick([&] { ++Calls; });
	Tick();
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_FALSE(Timers.IsTimerActive(Old));
	EXPECT_EQ(Timers.SetTimerForNextTick([] {}).Generation, 0u);
	ASSERT_TRUE(World->BeginPlay({}));
	const auto Fresh = Timers.SetTimerForNextTick([&] { ++Calls; });
	EXPECT_FALSE(Timers.ClearTimer(Old));
	Tick();
	EXPECT_EQ(Calls, 1);
	EXPECT_FALSE(Timers.IsTimerActive(Fresh));
}

TEST_F(FWorldTimerTests, TransitionAndShutdownFromCallbacksStopForwardDispatch)
{
	auto& Timers = World->GetTimerManager();
	auto* Next = Durin::NewObject<Durin::DLevel>(World, "TimerNextLevel");
	int Calls = 0;
	Timers.SetTimerForNextTick([&] { EXPECT_TRUE(World->RequestLevelTransition(Next)); });
	Timers.SetTimerForNextTick([&] { ++Calls; });
	Tick();
	EXPECT_EQ(Calls, 0);
	Tick();
	EXPECT_EQ(World->GetCurrentLevel(), Next);
	EXPECT_EQ(Calls, 0);
	Timers.SetTimerForNextTick([&] { World->Shutdown(); });
	Timers.SetTimerForNextTick([&] { ++Calls; });
	Tick();
	EXPECT_EQ(World->GetSubsystemState(), Durin::EWorldSubsystemState::Shutdown);
	EXPECT_EQ(Calls, 0);
}

TEST_F(FWorldTimerTests, ExceptionCancelsOffenderAndLeavesRemainingBatchUsable)
{
	auto& Timers = World->GetTimerManager();
	const auto Bad = Timers.SetTimer(0.0, [] { throw std::runtime_error("timer test"); }, 1.0);
	int Calls = 0;
	Timers.SetTimerForNextTick([&] { ++Calls; });
	EXPECT_THROW(Tick(), std::runtime_error);
	EXPECT_FALSE(Timers.IsTimerActive(Bad));
	EXPECT_EQ(Calls, 0);
	Tick();
	EXPECT_EQ(Calls, 1);
}

TEST_F(FWorldTimerTests, PausingAndResumingADueSiblingDefersItsOldQueueEntry)
{
	auto& Timers = World->GetTimerManager();
	Durin::FTimerHandle Sibling;
	int Calls = 0;
	Timers.SetTimerForNextTick([&]
	{
		EXPECT_TRUE(Timers.PauseTimer(Sibling));
		EXPECT_TRUE(Timers.UnpauseTimer(Sibling));
		Timers.SetTimerForNextTick([&] { ++Calls; });
		World->Tick({.DeltaSeconds = 100.0f}); // Reentry cannot admit the newly queued work.
	});
	Sibling = Timers.SetTimerForNextTick([&] { ++Calls; });
	Tick();
	EXPECT_EQ(Calls, 0);
	EXPECT_EQ(Timers.GetGameTimeSeconds(), 0.0);
	Tick();
	EXPECT_EQ(Calls, 2);
}

TEST_F(FWorldTimerTests, BoundCallbackCanDestroyItselfAndRequestGarbageCollection)
{
	auto& Timers = World->GetTimerManager();
	auto* Actor = World->SpawnActor<Durin::AActor>();
	const auto Identity = Durin::FObjectKey(Actor);
	int Calls = 0;
	const auto Handle = Timers.SetTimerForObject(Actor, 0.0, [&](Durin::DObject& Target)
	{
		++Calls;
		EXPECT_EQ(&Target, Actor);
		EXPECT_TRUE(World->DestroyActor(Actor));
		Durin::CollectGarbage();
		EXPECT_EQ(Durin::GDObjectArray.Resolve(Identity), &Target);
	}, 1.0);
	Tick();
	EXPECT_EQ(Calls, 1);
	EXPECT_FALSE(Timers.IsTimerActive(Handle));
	Tick(1.0f);
	EXPECT_EQ(Calls, 1);
}

TEST_F(FWorldTimerTests, PhysicsAndFrameTailObserveTheSameScaledStep)
{
	auto* Actor = World->SpawnActor<Durin::AStaticMeshActor>();
	auto* Physics = Durin::Cast<Durin::DPhysicsComponent>(
		Actor->AddInstanceComponent(Durin::DPhysicsComponent::StaticClass(), "TimerPhysics"));
	ASSERT_NE(Physics, nullptr);
	Actor->GetRootComponent()->SetWorldLocation({0.0, 0.0, 10.0});
	World->SetTimeScale(0.5f);
	bool bCalled = false;
	World->GetTimerManager().SetTimer(0.5, [&]
	{
		bCalled = true;
		EXPECT_NEAR(Physics->GetLinearVelocity().z, -9.81 * 0.5, 0.0001);
		EXPECT_NEAR(Actor->GetActorTransform().Translation.z, 10.0 - 9.81 * 0.25, 0.0001);
	});
	Tick(1.0f);
	EXPECT_TRUE(bCalled);
}

TEST_F(FWorldTimerTests, BeginPlayCanRegisterBoundTimersBeforeTheFirstFrame)
{
	World->EndPlay();
	auto* Actor = World->SpawnActor<Durin::FTimerTestActor>();
	int Calls = 0;
	Actor->OnBegin = [&]
	{
		const auto Handle = World->GetTimerManager().SetTimerForObjectNextTick(Actor,
			[&](Durin::DObject& Target) { EXPECT_EQ(&Target, Actor); ++Calls; });
		EXPECT_NE(Handle.Generation, 0u);
	};
	ASSERT_TRUE(World->BeginPlay({}));
	EXPECT_EQ(Calls, 0);
	Tick();
	EXPECT_EQ(Calls, 1);
}

TEST_F(FWorldTimerTests, EndPlayClosesOwnerAdmissionBeforeReleasingTimerCaptures)
{
	auto* Actor = World->SpawnActor<Durin::AActor>();
	auto& Timers = World->GetTimerManager();
	bool bReleased = false;
	auto Capture = std::shared_ptr<int>(new int(1), [&](int* Value)
	{
		delete Value;
		bReleased = true;
		EXPECT_EQ(Timers.SetTimerForObjectNextTick(Actor, [](Durin::DObject&) {}).Generation, 0u);
	});
	Timers.SetTimerForObject(Actor, 100.0, [Capture](Durin::DObject&) {});
	Capture.reset();
	Actor->RouteEndPlay();
	EXPECT_TRUE(bReleased);
	Actor->DispatchBeginPlay();
	Tick();
}
