#include "Editor/TransactionRecordTestSupport.h"
#include "DObject/ObjectGraphReplacement.h"

using namespace Durin::Tests::ObjectState;

TEST(FEngineTransactionAnnotationTests, ActorRecordRestoresGeneratedOverrideAndNotifiesOnlyActor)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedEvents"); ASSERT_NE(Actor, nullptr);
	auto Scene = [&] { return static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0))); };
	ASSERT_NE(Scene(), nullptr);
	ASSERT_EQ(Scene()->GetRelativeLocation(), FVector3(0.0));
	std::vector<ETransactionObjectEventType> Events;
	Actor->TransactionObserver = [&](const auto& Event) {
		EXPECT_TRUE(Event.Annotation);
		EXPECT_NE(Event.TransactionId, 0u); EXPECT_NE(Event.OperationId, 0u);
		EXPECT_FALSE(std::ranges::contains(Event.Change.ChangedProperties, FName("RelativeTransform")));
		Events.push_back(Event.Type);
	};
	Actor->GeneratedTransactionObserver = [](auto&, const auto&) { ADD_FAILURE() << "Actor record notified a generated component"; };
	Scene()->TransactionObserver = Actor->GeneratedTransactionObserver;
	const auto Retired = FObjectKey(Scene());
	ASSERT_EQ(Buffer->Begin("test", "Generated override"), 0); ASSERT_TRUE(Actor->Modify());
	Scene()->SetRelativeLocation({12.0, 0.0, 0.0});
	ASSERT_TRUE(Actor->RerunConstructionScripts());
	EXPECT_TRUE(Events.empty()); EXPECT_EQ(Buffer->End(), 1);
	EXPECT_EQ(Events, std::vector{ETransactionObjectEventType::Finalized});
	CollectGarbage(EObjectFlags::Standalone); EXPECT_EQ(ResolveObjectKey(Retired), nullptr);
	for (int Cycle = 0; Cycle < 2; ++Cycle)
	{
		Events.clear();
		ASSERT_TRUE(Buffer->Undo()); ASSERT_NE(Scene(), nullptr);
		EXPECT_EQ(Scene()->GetRelativeLocation(), FVector3(0.0));
		ASSERT_TRUE(Buffer->Redo()); ASSERT_NE(Scene(), nullptr);
		EXPECT_EQ(Scene()->GetRelativeLocation(), FVector3(12.0, 0.0, 0.0));
		EXPECT_EQ(Events, (std::vector{ETransactionObjectEventType::UndoRedo, ETransactionObjectEventType::UndoRedo}));
	}
	Actor->TransactionObserver = {};
}

TEST(FTransBufferTests, AnnotationOnlyHistoryRestoresDesiredSideAndFlipsLiveSupplementalState)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationOnlyHistory"));
	Target->bAnnotationEnabled = true;
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Annotation only"}); ASSERT_TRUE(Target->Modify()); Target->SupplementalValue = 9; }
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	Target->SupplementalValue = 17;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->SupplementalValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->SupplementalValue, 17);
	EXPECT_EQ(Target->AnnotationCompletions, 2); EXPECT_EQ(Target->UndoBegins, 2); EXPECT_EQ(Target->UndoCompletions, 2);
}

TEST(FTransBufferTests, AnnotationReferencesSurviveCallbackCollectionAndReleaseWithHistory)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationHistoryReferences"));
	Target->bAnnotationEnabled = true; Target->bRequestCollectionDuringAnnotation = true;
	Target->AnnotationReference = NewObject<DObject>(nullptr, "AnnotationRetainedReference");
	const auto Key = FObjectKey(Target->AnnotationReference);
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Annotation GC"}); ASSERT_TRUE(Target->Modify()); Target->SupplementalValue = 3; }
	Target->AnnotationReference = nullptr; CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_NE(ResolveObjectKey(Key), nullptr);
	ASSERT_TRUE(Buffer->Undo()); CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_NE(ResolveObjectKey(Key), nullptr);
	Buffer->Reset(); CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_EQ(ResolveObjectKey(Key), nullptr);
}

TEST(FTransBufferTests, AnnotatedLoadFaultTerminatesUndo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CompensatedAnnotation"));
	Target->bAnnotationEnabled = true; Target->SupplementalValue = 1;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Annotation compensation"}); ASSERT_TRUE(Target->Modify());
		Target->Value = 7; Target->SupplementalValue = 2;
	}
	Target->bFailLoadOnce = true;
	EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
}

TEST(FTransBufferTests, LaterRecordFaultTerminatesAnnotatedUndo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotatedAtomicRetry"));
	Target->bAnnotationEnabled = true;
	int OpaqueValue = 0;
	auto Failure = std::make_unique<FTestCustomChange>(OpaqueValue, 0, 1); auto* Fault = Failure.get();
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Annotated atomic retry"});
		ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Failure)));
		ASSERT_TRUE(Target->Modify()); Target->Value = 7; Target->SupplementalValue = 3;
	}
	Target->SupplementalValue = 11; Fault->bFailUndo = true;
	EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
}

TEST(FTransBufferTests, WholeAnnotationKeepsFirstStateThroughRepeatedCapture)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "WholeAnnotation"));
	Target->bAnnotationEnabled = true; Target->SupplementalValue = 1;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Whole annotation capture"});
		ASSERT_TRUE(Target->Modify(false));
		Target->Value = 2; Target->SupplementalValue = 7;
		ASSERT_TRUE(Target->Modify(false));
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SupplementalValue, 1);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 2); EXPECT_EQ(Target->SupplementalValue, 7);
}

TEST(FEngineTransactionAnnotationTests, StandaloneComponentRecordsAuthoredFieldsWithoutLifecycleCheckpoints)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionLifecycleScene> Component(NewObject<DTransactionLifecycleScene>(nullptr, "ComponentHistory"));
	Component->SetFlags(EObjectFlags::Transactional);
	{
		Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Change native Tick"});
		ASSERT_TRUE(Component->Modify());
		Component->SetComponentTickEnabled(true);
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	std::vector<ETransactionObjectEventType> Events;
	Component->TransactionObserver = [&](auto& Target, const FTransactionObjectEvent& Event) {
		EXPECT_EQ(&Target, Component.Get()); EXPECT_FALSE(Event.Annotation);
		EXPECT_EQ(Event.Change.ChangedProperties, std::vector<FName>{"RelativeTransform"});
		Events.push_back(Event.Type);
	};
	const auto Before = Component->GetRelativeLocation();
	{
		Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Move component"});
		ASSERT_TRUE(Component->Modify());
		Component->SetRelativeLocation({17.0, 0.0, 0.0});
		Component->SetComponentTickEnabled(false);
	}
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Component->GetRelativeLocation(), Before); EXPECT_FALSE(Component->IsComponentTickEnabled());
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Component->GetRelativeLocation().x, 17.0);
	EXPECT_EQ(Events, (std::vector{ETransactionObjectEventType::Finalized, ETransactionObjectEventType::UndoRedo, ETransactionObjectEventType::UndoRedo}));
}

TEST(FEngineTransactionAnnotationTests, ActorReplayUsesNormalComponentRegistrationAndPreservesLiveComponentTick)
{
	using namespace Durin; using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("NativeAnnotationActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent()); ASSERT_NE(Root, nullptr);
	ASSERT_EQ(Root->Created, 1u); ASSERT_EQ(Root->Registered, 1u);
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Native annotation lifecycle"}); ASSERT_TRUE(Actor->Modify());
		Actor->SetActorTickEnabled(true); Root->SetComponentTickEnabled(true); Root->UnregisterComponent();
	}
	const auto Constructions = Actor->ConstructionCalls;
	Root->bRequestCollectionOnRegister = true;
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_TRUE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Root->IsComponentTickEnabled()); EXPECT_TRUE(Root->IsRegistered());
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u); EXPECT_EQ(Root->Initialized, 1u);
	EXPECT_EQ(Actor->ConstructionCalls, Constructions + 1);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_TRUE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Root->IsComponentTickEnabled()); EXPECT_TRUE(Root->IsRegistered());
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u); EXPECT_EQ(Root->Unregistered, 1u); EXPECT_EQ(Root->Uninitialized, 0u);
	EXPECT_EQ(Actor->ConstructionCalls, Constructions + 2);
}

TEST(FEngineTransactionAnnotationTests, GeneratedRootRecreatesWithoutRetainingOldGenerationAndRestoresExternalAttachment)
{
	using namespace Durin; using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedRootActor"); ASSERT_NE(Actor, nullptr);
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachedActor"); ASSERT_NE(Other, nullptr);
	auto* Generated = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Generated, nullptr); ASSERT_TRUE(Actor->SetRootComponent(Generated));
	Generated->SetRelativeLocation({5.0, 0.0, 0.0});
	ASSERT_TRUE(Other->GetRootComponent()->AttachToComponent(Generated, EAttachmentTransformRule::KeepRelative));
	Other->GetRootComponent()->SetRelativeLocation({2.0, 0.0, 0.0});
	const auto BeforeWorld = Generated->GetWorldLocation(); const auto ChildWorld = Other->GetRootComponent()->GetWorldLocation();
	const auto OldKey = FObjectKey(Generated);
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Generated root state"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	EXPECT_EQ(Actor->GetRootComponent(), nullptr); CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	Actor->bRequestCollectionDuringConstruction = true;
	const auto DetachedPose = Other->GetRootComponent()->GetWorldLocation();
	for (int Cycle = 0; Cycle < 2; ++Cycle)
	{
		ASSERT_TRUE(Buffer->Undo());
		auto* Restored = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent()); ASSERT_NE(Restored, nullptr);
		EXPECT_NE(FObjectKey(Restored), OldKey); EXPECT_EQ(Restored->GetWorldLocation(), BeforeWorld); EXPECT_EQ(Restored->CreatedLocation, FVector3(0.0));
		EXPECT_EQ(Restored->Created, 1u); EXPECT_EQ(Restored->Registered, 1u); EXPECT_EQ(Restored->Initialized, 1u);
		EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), Restored); EXPECT_EQ(Other->GetRootComponent()->GetWorldLocation(), ChildWorld);
		ASSERT_TRUE(Buffer->Redo());
		EXPECT_EQ(Actor->GetRootComponent(), nullptr);
		EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), nullptr);
		EXPECT_EQ(Other->GetRootComponent()->GetWorldLocation(), DetachedPose);
	}
}

TEST(FEngineTransactionAnnotationTests, SerializedChildEditsAndGeneratedRootReconstructionPreserveBothPoses)
{
	using namespace Durin; using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("MixedRootActor");
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("MixedRootChild");
	ASSERT_NE(Actor, nullptr); ASSERT_NE(Other, nullptr);
	auto* Root = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_TRUE(Actor->SetRootComponent(Root)); Root->SetRelativeLocation({5.0, 0.0, 0.0});
	auto* Child = Other->GetRootComponent(); ASSERT_TRUE(Child->AttachToComponent(Root)); Child->SetRelativeLocation({2.0, 0.0, 0.0});
	const auto Before = Child->GetWorldLocation();
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Serialized child and root reconstruction"});
		ASSERT_TRUE(Actor->Modify()); ASSERT_TRUE(Child->Modify()); Child->SetRelativeLocation({3.0, 0.0, 0.0});
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	const auto After = Child->GetWorldLocation(); EXPECT_EQ(After, FVector3(8.0, 0.0, 0.0));
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Child->GetWorldLocation(), Before);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Child->GetWorldLocation(), After);
}

TEST(FEngineTransactionAnnotationTests, SwitchingGeneratedAndAuthoredRootsPreservesBothHistorySides)
{
	using namespace Durin; using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("RootKindSwitchActor"); ASSERT_NE(Actor, nullptr);
	auto* NativeRoot = Actor->GetRootComponent();
	auto* GeneratedRoot = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(GeneratedRoot, nullptr); ASSERT_TRUE(Actor->SetRootComponent(GeneratedRoot));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Root kind switch"}); ASSERT_TRUE(Actor->Modify());
		ASSERT_TRUE(Actor->SetRootComponent(NativeRoot));
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Actor->GetRootComponent(), Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	EXPECT_NE(Actor->GetRootComponent(), GeneratedRoot); EXPECT_TRUE(GeneratedRoot->IsPendingKill());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Actor->GetRootComponent(), NativeRoot);
}

TEST(FEngineTransactionAnnotationTests, ReconstructionFaultIsTerminal)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("FailedReconstructionActor"); ASSERT_NE(Actor, nullptr);
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Reconstruction failure"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 2; ASSERT_TRUE(Actor->RerunConstructionScripts());
	} Actor->bFailConstruction = true;
	EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
}

TEST(FEngineTransactionAnnotationTests, ExpiredAnnotationParticipantRejectsBeforeSerializedMutation)
{
	using namespace Durin; using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("AnnotationExpiryActor");
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("AnnotationExpiryChild");
	ASSERT_NE(Actor, nullptr); ASSERT_NE(Other, nullptr);
	auto* Generated = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_TRUE(Actor->SetRootComponent(Generated)); ASSERT_TRUE(Other->GetRootComponent()->AttachToComponent(Generated));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Expired annotation participant"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	MarkAsGarbage(Other->GetRootComponent());
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Actor->GeneratedCount, 0); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_TRUE(Buffer->CanTransact());
	EXPECT_FALSE(Buffer->IsActive());
}

TEST(FEngineTransactionAnnotationTests, RegistrationCallbackCanRetireComponentDuringUndo)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("CallbackRetirementActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Retiring callback"}); ASSERT_TRUE(Actor->Modify()); Root->UnregisterComponent();
	}
	Root->bDestroyOnRegister = true;
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_TRUE(Root->IsPendingKill());
	EXPECT_FALSE(Actor->OwnsComponent(Root));
}

TEST(FEngineTransactionAnnotationTests, GeneratedSceneOverridesReplayWithoutAnAuthoredPropertyRecord)
{
	using namespace Durin; using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedPoseReplay"); ASSERT_NE(Actor, nullptr);
	auto* Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); Scene->SetRelativeLocation({9.0, 0.0, 0.0});
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Generated scene pose"}); ASSERT_TRUE(Actor->Modify());
		Scene->SetRelativeLocation({12.0, 0.0, 0.0}); ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	ASSERT_TRUE(Buffer->Undo());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), FVector3(9.0, 0.0, 0.0));
	ASSERT_TRUE(Buffer->Redo());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), FVector3(12.0, 0.0, 0.0));
	Actor->ConstructionLocation = FVector3(12.0, 0.0, 0.0);
	ASSERT_TRUE(Buffer->Undo());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), *Actor->ConstructionLocation);
	ASSERT_TRUE(Buffer->Redo());
	Actor->ConstructionLocation = FVector3(4.0, 0.0, 0.0);
	ASSERT_TRUE(Buffer->Undo());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), *Actor->ConstructionLocation);
}

TEST(FEngineTransactionAnnotationTests, ActorTickGroupUsesPropertyHistoryAndRuntimeTickDoesNot)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<AActor>("SparseActorAnnotation"); ASSERT_NE(Actor, nullptr);
	auto* Member = Actor->GetClass()->FindPropertyByName("PrimaryActorTick");
	ASSERT_NE(Member, nullptr);
	EXPECT_FALSE(Actor->CanEditChange(Member));
	EXPECT_TRUE(Actor->GetClass()->GetDefaultObject()->CanEditChange(Member));
	ASSERT_TRUE(Actor->PrimaryActorTick.IsTickFunctionRegistered());
	EXPECT_FALSE(Actor->FindOrCreateTransactionAnnotation());
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Actor tick state"}); ASSERT_TRUE(Actor->Modify());
		Actor->SetActorTickEnabled(true);
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	{
		Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Actor tick group"}); ASSERT_TRUE(Actor->Modify());
		Actor->PrimaryActorTick.TickGroup = ETickingGroup::PostPhysics;
	}
	EXPECT_FALSE(Actor->FindOrCreateTransactionAnnotation());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Actor->PrimaryActorTick.TickGroup, ETickingGroup::PrePhysics);
	EXPECT_TRUE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Actor->PrimaryActorTick.IsTickFunctionRegistered());
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Actor->PrimaryActorTick.TickGroup, ETickingGroup::PostPhysics);
	EXPECT_TRUE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Actor->PrimaryActorTick.IsTickFunctionRegistered());
}

TEST(FEngineTransactionAnnotationTests, InnerRegistrationFaultTerminatesUndo)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("InnerCallbackFailure");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Inner registration failure"}); ASSERT_TRUE(Actor->Modify());
		Root->UnregisterComponent();
	}
	Root->bThrowOnRegister = true;
	EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
}

TEST(FEngineTransactionAnnotationTests, AttachmentReverseLinksAreSettledBeforeHistoricalRegistrationCallback)
{
	using namespace Durin; using namespace Durin;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackChild");
	auto* First = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackFirst");
	auto* Second = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackSecond");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	ASSERT_TRUE(Root->AttachToComponent(First->GetRootComponent()));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Attachment callback state"}); ASSERT_TRUE(Actor->Modify()); ASSERT_TRUE(Root->Modify());
		ASSERT_TRUE(Root->AttachToComponent(Second->GetRootComponent())); Root->UnregisterComponent();
	}
	bool Called = false;
	Root->RegistrationCallback = [&] {
		Called = true;
		EXPECT_EQ(Root->GetAttachParent(), First->GetRootComponent());
		EXPECT_EQ(std::ranges::count_if(First->GetRootComponent()->GetAttachChildren(), [&](const auto& Child) { return Child.Get() == Root; }), 1);
		EXPECT_EQ(std::ranges::count_if(Second->GetRootComponent()->GetAttachChildren(), [&](const auto& Child) { return Child.Get() == Root; }), 0);
	};
	ASSERT_TRUE(Buffer->Undo()); EXPECT_TRUE(Called); Root->RegistrationCallback = {};
}

TEST(FEngineTransactionAnnotationTests, IncompatibleActorAnnotationReleasesSharedSuspension)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("WrongAnnotationSuspension");
	Actor->PreEditUndo();
	EXPECT_THROW(Actor->PostEditUndo(std::make_shared<FRecordTestAnnotation>(0, nullptr)), std::runtime_error);
	const auto Before = Actor->ConstructionCalls;
	ASSERT_TRUE(Actor->RerunConstructionScripts()); EXPECT_EQ(Actor->ConstructionCalls, Before + 1);
}

TEST(FEngineTransactionAnnotationTests, DerivedInstanceDataAppliesAndCopiesItsExactType)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("TypedCacheActor"); ASSERT_NE(Actor, nullptr);
	auto* Component = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Component, nullptr); Component->bTypedInstanceData = true;
	FComponentInstanceDataCache Original(*Actor);
	FTypedSceneInstanceData::AppliedValue = 0;
	ASSERT_TRUE(Original.Validate(Actor));
	Original.ApplyToActor(*Actor);
	EXPECT_EQ(FTypedSceneInstanceData::AppliedValue, 42);
	FObjectReplacementMap Map;
	auto Data = Component->GetComponentInstanceData(); auto Copy = Data->CreateReplacement(Map);
	ASSERT_TRUE(Copy) << Copy.error(); ASSERT_NE(Copy->get(), Data.get());
	auto* TypedCopy = dynamic_cast<FTypedSceneInstanceData*>(Copy->get()); ASSERT_NE(TypedCopy, nullptr);
	EXPECT_EQ(TypedCopy->DomainValue, 42); TypedCopy->DomainValue = 91;
	EXPECT_EQ(static_cast<FTypedSceneInstanceData*>(Data.get())->DomainValue, 42);
}
