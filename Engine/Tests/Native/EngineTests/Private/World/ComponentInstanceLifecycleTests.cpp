#include "World/ObjectStateTestSupport.h"
#include "ComponentReregisterContext.h"

using namespace Durin::Tests::ObjectState;

TEST(FComponentInstanceLifecycleTests, OrdinaryReconstructionConsumesTheSameGeneratedRootCache)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("OrdinaryCachedRoot");
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("OrdinaryCachedChild");
	ASSERT_NE(Actor, nullptr); ASSERT_NE(Other, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_TRUE(Actor->SetRootComponent(Root)); Root->SetRelativeLocation({5.0, 0.0, 0.0});
	ASSERT_TRUE(Other->GetRootComponent()->AttachToComponent(Root)); Other->GetRootComponent()->SetRelativeLocation({2.0, 0.0, 0.0});
	const auto RootPose = Root->GetWorldLocation(), ChildPose = Other->GetRootComponent()->GetWorldLocation();
	const auto OldKey = FObjectKey(Root);
	FComponentInstanceDataCache InstanceData(*Actor);
	FActorRootComponentReconstructionData RootData(*Actor);
	Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	CollectGarbage(); EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	Actor->GeneratedCount = 1; Actor->bCollectDuringConstruction = true;
	ASSERT_TRUE(Actor->RerunConstructionScripts(&InstanceData, &RootData));
	auto* Restored = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent()); ASSERT_NE(Restored, nullptr);
	EXPECT_NE(FObjectKey(Restored), OldKey); EXPECT_EQ(Restored->GetWorldLocation(), RootPose);
	EXPECT_EQ(Restored->CreatedLocation, FVector3(0.0)); EXPECT_EQ(Restored->Created, 1u); EXPECT_EQ(Restored->Registered, 1u);
	EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), Restored); EXPECT_EQ(Other->GetRootComponent()->GetWorldLocation(), ChildPose);
}

TEST(FComponentInstanceLifecycleTests, InstanceDataFollowsConstructionAndCreationCallbacks)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("CandidateInstancePose"); ASSERT_NE(Actor, nullptr);
	auto* Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); Scene->SetRelativeLocation({9.0, 0.0, 0.0});
	FComponentInstanceDataCache InstanceData(*Actor);
	Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	struct FResetNativePose { ~FResetNativePose() { DTransactionLifecycleScene::NativeInitialLocation = FVector3(0.0); } } Reset;
	DTransactionLifecycleScene::NativeInitialLocation = FVector3(2.0, 0.0, 0.0);
	Actor->GeneratedCount = 1; ASSERT_TRUE(Actor->RerunConstructionScripts(&InstanceData));
	auto* Restored = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Restored, nullptr);
	EXPECT_EQ(Restored->GetRelativeLocation(), FVector3(9.0, 0.0, 0.0));
	EXPECT_EQ(Restored->CreatedLocation, FVector3(0.0));
	EXPECT_EQ(Restored->Created, 1u); EXPECT_EQ(Restored->Registered, 1u);
}

TEST(FComponentInstanceLifecycleTests, OrdinaryReregistrationPreservesPlayInitializationTickAndTimers)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture(EWorldType::Game);
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("OrdinaryReregister"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	ASSERT_TRUE(Fixture.World->BeginPlay({}));
	ASSERT_TRUE(Root->SetComponentTickGroup(ETickingGroup::PostPhysics)); Root->SetComponentTickEnabled(true);
	const auto Before = Root->CaptureLifecycleState();
	int TimerCalls = 0;
	const auto Timer = Fixture.World->GetTimerManager().SetTimerForObjectNextTick(Root, [&](DObject&) { ++TimerCalls; });
	{
		FComponentReregisterContext Outer(Root);
		EXPECT_FALSE(Root->IsRegistered()); EXPECT_TRUE(Root->HasBeenInitialized()); EXPECT_TRUE(Root->HasBegunPlay());
		{ FComponentReregisterContext Nested(Root); EXPECT_FALSE(Root->IsRegistered()); }
		EXPECT_FALSE(Root->IsRegistered());
		ASSERT_TRUE(Outer.ReRegister()); ASSERT_TRUE(Outer.ReRegister());
	}
	EXPECT_EQ(Root->CaptureLifecycleState(), Before);
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u); EXPECT_EQ(Root->Unregistered, 1u);
	EXPECT_EQ(Root->Initialized, 1u); EXPECT_EQ(Root->Uninitialized, 0u); EXPECT_EQ(Root->Begun, 1u); EXPECT_EQ(Root->Ended, 0u);
	EXPECT_TRUE(Fixture.World->GetTimerManager().IsTimerActive(Timer));
	Fixture.World->Tick({.DeltaSeconds = 0.1f}); EXPECT_EQ(TimerCalls, 1);
	Root->ReregisterComponent(); EXPECT_EQ(Root->Registered, 3u); EXPECT_EQ(Root->Initialized, 1u); EXPECT_EQ(Root->Begun, 1u);
	Fixture.World->EndPlay(); EXPECT_EQ(Root->Ended, 1u);
}

TEST(FComponentInstanceLifecycleTests, SharedLifecycleRestoresUnregisteredInitializedStateWithoutCreationDuplication)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("UnregisteredInitialized");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	Root->UnregisterComponent(); ASSERT_TRUE(Root->HasBeenInitialized());
	const auto Target = Root->CaptureLifecycleState();
	ASSERT_TRUE(Root->ApplyLifecycleState({}));
	EXPECT_FALSE(Root->HasBeenCreated()); EXPECT_FALSE(Root->HasBeenInitialized());
	ASSERT_TRUE(Root->ApplyLifecycleState(Target)); EXPECT_EQ(Root->CaptureLifecycleState(), Target);
	EXPECT_EQ(Root->Created, 2u); EXPECT_EQ(Root->Initialized, 2u);
	const auto Registered = Root->Registered;
	ASSERT_TRUE(Root->ApplyLifecycleState(Target)); EXPECT_EQ(Root->Registered, Registered);
}

TEST(FComponentInstanceLifecycleTests, ReregisterScopeDoesNotResurrectRetiredComponentAndDefersCollection)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("RetiredReregister");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	const FObjectKey Key(Root);
	{
		FComponentReregisterContext Context(Root);
		Root->DestroyComponent(); CollectGarbage();
		EXPECT_TRUE(Root->IsPendingKill()); EXPECT_FALSE(Root->IsRegistered());
		ASSERT_TRUE(Context.ReRegister()); EXPECT_EQ(Root->Registered, 1u);
	}
	CollectGarbage(); EXPECT_EQ(ResolveObjectKey(Key), nullptr);
}

TEST(FComponentInstanceLifecycleTests, ReregisterCallbackFailureIsReportedAndConsumedWithoutRetry)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("ThrowingReregister");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		FComponentReregisterContext Context(Root);
		Root->bThrowOnRegister = true;
		const auto Result = Context.ReRegister(); ASSERT_FALSE(Result);
		EXPECT_NE(Result.error().find("Injected registration"), std::string::npos);
		Root->bThrowOnRegister = false;
	}
	EXPECT_EQ(Root->Registered, 2u); // A partially executed callback is never retried implicitly.
	Root->UnregisterComponent(); Root->RegisterComponent(); EXPECT_TRUE(Root->IsRegistered());
}

TEST(FComponentInstanceLifecycleTests, ReregisterScopePreservesTickConfigurationEdits)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("ReregisterTickEdit");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		FComponentReregisterContext Context(Root);
		ASSERT_TRUE(Root->SetComponentTickGroup(ETickingGroup::PostPhysics)); Root->SetComponentTickEnabled(true);
	}
	EXPECT_EQ(Root->GetPrimaryComponentTick().GetTickGroup(), ETickingGroup::PostPhysics);
	EXPECT_TRUE(Root->IsComponentTickEnabled()); EXPECT_TRUE(Root->GetPrimaryComponentTick().IsTickFunctionRegistered());
	EXPECT_EQ(Root->Initialized, 1u); EXPECT_EQ(Root->Registered, 2u);
}
