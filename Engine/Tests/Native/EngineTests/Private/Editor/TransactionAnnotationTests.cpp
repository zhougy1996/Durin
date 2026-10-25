#include "Editor/TransactionRecordTestSupport.h"
#include "DObject/ObjectGraphReplacement.h"

using namespace Durin::Tests::ObjectState;

TEST(FEngineTransactionAnnotationTests, GeneratedComponentEventsFollowSnapshotsAndReconstructedReplayInstances)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedEvents"); ASSERT_NE(Actor, nullptr);
	auto Scene = [&] { return static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0))); };
	std::vector<FTransactionObjectEvent> Events;
	Actor->GeneratedTransactionObserver = [&](auto& Component, const auto& Event) {
		EXPECT_EQ(&Component, Scene()); EXPECT_TRUE(Component.IsRegistered());
		EXPECT_EQ(Event.CreatedBy, ETransactionObjectChangeCreatedBy::TransactionAnnotation);
		EXPECT_EQ(Event.Change.ChangedProperties, std::vector<FName>{"RelativeTransform"});
		EXPECT_NE(Event.TransactionId, 0u); EXPECT_NE(Event.OperationId, 0u);
		Events.push_back(Event);
	};
	Scene()->TransactionObserver = Actor->GeneratedTransactionObserver;
	const auto Retired = FObjectKey(Scene());
	ASSERT_EQ(Buffer->Begin("test", "Generated events"), 0); ASSERT_TRUE(Actor->Modify());
	Scene()->SetRelativeLocation({12.0, 0.0, 0.0});
	ASSERT_TRUE(GUndo->SnapshotObject(Actor)); ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Snapshot);
	ASSERT_TRUE(Actor->RerunConstructionScripts());
	EXPECT_EQ(Buffer->End(), 1); ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Finalized);
	CollectGarbage(); EXPECT_EQ(ResolveObjectKey(Retired), nullptr);
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	ASSERT_EQ(Events.size(), 3u); EXPECT_EQ(Events.back().Operation, ETransactionOperation::Undo);
	EXPECT_EQ(Scene()->GetRelativeLocation(), FVector3(0.0));
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	ASSERT_EQ(Events.size(), 4u); EXPECT_EQ(Events.back().Operation, ETransactionOperation::Redo);
	EXPECT_EQ(Scene()->GetRelativeLocation(), FVector3(12.0, 0.0, 0.0));
	const auto Bytes = Buffer->GetUndoSize();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo());
		EXPECT_EQ(Buffer->GetUndoSize(), Bytes);
	}
	Actor->GeneratedTransactionObserver = {}; Scene()->TransactionObserver = {};
	ASSERT_TRUE(Buffer->TryReset()); EXPECT_EQ(Buffer->GetUndoSize(), 0u);
}

TEST(FEngineTransactionAnnotationTests, GeneratedComponentFinalEventClosesLastSnapshotAtOriginalValue)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedSnapshotClosure");
	auto* Scene = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	std::vector<FTransactionObjectEvent> Events;
	Scene->TransactionObserver = [&](auto&, const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Generated closure"), 0); ASSERT_TRUE(Actor->Modify());
	Scene->SetRelativeLocation({7.0, 0.0, 0.0}); ASSERT_TRUE(GUndo->SnapshotObject(Actor));
	ASSERT_TRUE(GUndo->SnapshotObject(Actor)); EXPECT_EQ(Events.size(), 1u);
	Scene->SetRelativeLocation({0.0, 0.0, 0.0}); EXPECT_EQ(Buffer->End(), 1);
	ASSERT_EQ(Events.size(), 2u); EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Events.back().Change.ChangedProperties, std::vector<FName>{"RelativeTransform"});
	EXPECT_EQ(Events.back().CreatedBy, ETransactionObjectChangeCreatedBy::TransactionAnnotation);
	Scene->TransactionObserver = {};
}

TEST(FEngineTransactionAnnotationTests, ComponentObservationFiltersConstructionTransformsAndReportsLiveAdditions)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedObservationPolicy");
	FActorTransactionAnnotation Before(*Actor);
	Actor->ConstructionLocation = FVector3(9.0, 0.0, 0.0); Actor->GeneratedCount = 2;
	ASSERT_TRUE(Actor->RerunConstructionScripts());
	FActorTransactionAnnotation After(*Actor);
	auto Changes = After.ComputeAdditionalObjectChanges(&Before);
	ASSERT_EQ(Changes.size(), 1u);
	EXPECT_EQ(Changes[0].first, Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(1)));
	EXPECT_TRUE(Changes[0].second.bEditorPresenceChanged);
	EXPECT_FALSE(std::ranges::contains(Changes[0].second.ChangedProperties, FName("RelativeTransform")));
	EXPECT_FALSE(std::ranges::contains(Changes[0].second.ChangedProperties, FName("CreationMethod")));
	Actor->SetRootComponent(static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0))));
	Changes = After.ComputeAdditionalObjectChanges(&Before);
	EXPECT_TRUE(std::ranges::any_of(Changes, [](const auto& Change) { return std::ranges::contains(Change.second.ChangedProperties, FName("RelativeTransform")); }));
	Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	FActorTransactionAnnotation Removed(*Actor); EXPECT_TRUE(Removed.ComputeAdditionalObjectChanges(&After).empty());
}

TEST(FEngineTransactionAnnotationTests, GeneratedAttachmentReferencesUsePathsAndDoNotRetainRetiredGenerations)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedAttachmentEvents");
	Actor->GeneratedCount = 2; ASSERT_TRUE(Actor->RerunConstructionScripts());
	auto Parent = [&] { return static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0))); };
	auto Child = [&] { return static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(1))); };
	ASSERT_TRUE(Child()->AttachToComponent(Parent(), EAttachmentTransformRule::KeepRelative));
	ASSERT_EQ(Buffer->Begin("test", "Generated attachment observation"), 0); ASSERT_TRUE(Actor->Modify());
	const auto OldParent = FObjectKey(Parent()), OldChild = FObjectKey(Child());
	FActorTransactionAnnotation Before(*Actor);
	ASSERT_TRUE(Actor->RerunConstructionScripts());
	ASSERT_TRUE(Child()->AttachToComponent(Parent(), EAttachmentTransformRule::KeepRelative));
	FActorTransactionAnnotation After(*Actor);
	EXPECT_TRUE(After.ComputeAdditionalObjectChanges(&Before).empty());
	EXPECT_EQ(Buffer->End(), 1);
	CollectGarbage(); EXPECT_EQ(ResolveObjectKey(OldParent), nullptr); EXPECT_EQ(ResolveObjectKey(OldChild), nullptr);
	ASSERT_TRUE(Child()->DetachFromComponent(EDetachmentTransformRule::KeepRelative));
	FActorTransactionAnnotation Detached(*Actor);
	const auto Changes = Detached.ComputeAdditionalObjectChanges(&After);
	ASSERT_EQ(Changes.size(), 1u); EXPECT_EQ(Changes[0].first, Child());
	EXPECT_EQ(Changes[0].second.ChangedProperties, std::vector<FName>{"AttachParent"});
}

TEST(FTransBufferTests, AnnotationOnlyHistoryRestoresDesiredSideAndFlipsLiveSupplementalState)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationOnlyHistory"));
	Target->bAnnotationEnabled = true;
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Annotation only"}); ASSERT_TRUE(Target->Modify()); Target->SupplementalValue = 9; }
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	Target->SupplementalValue = 17;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->SupplementalValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->SupplementalValue, 17);
	EXPECT_EQ(Target->AnnotationCompletions, 2); EXPECT_EQ(Target->UndoBegins, 2); EXPECT_EQ(Target->UndoCompletions, 2);
}

TEST(FTransBufferTests, AnnotationReferencesSurviveCallbackCollectionAndReleaseWithHistory)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationHistoryReferences"));
	Target->bAnnotationEnabled = true; Target->bCollectDuringAnnotation = true;
	Target->AnnotationReference = NewObject<DObject>(nullptr, "AnnotationRetainedReference");
	const auto Key = FObjectKey(Target->AnnotationReference);
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Annotation GC"}); ASSERT_TRUE(Target->Modify()); Target->SupplementalValue = 3; }
	Target->AnnotationReference = nullptr; CollectGarbage(); EXPECT_NE(ResolveObjectKey(Key), nullptr);
	ASSERT_TRUE(Buffer->Undo()); CollectGarbage(); EXPECT_NE(ResolveObjectKey(Key), nullptr);
	ASSERT_TRUE(Buffer->TryReset()); CollectGarbage(); EXPECT_EQ(ResolveObjectKey(Key), nullptr);
}

TEST(FTransBufferTests, CompensatedLoadCompletesWithPreOperationAnnotation)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CompensatedAnnotation"));
	Target->bAnnotationEnabled = true; Target->SupplementalValue = 1;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Annotation compensation"}); ASSERT_TRUE(Target->Modify());
		Target->Value = 7; Target->SupplementalValue = 2;
	}
	Target->bFailLoadOnce = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Target->SupplementalValue, 2); EXPECT_EQ(Target->Value, 7);
	EXPECT_EQ(Target->AnnotationCompletions, 1); EXPECT_EQ(Buffer->GetUndoCount(), 0u); EXPECT_TRUE(Buffer->CanUndo());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->SupplementalValue, 1); EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->SupplementalValue, 2); EXPECT_EQ(Target->Value, 7);
	EXPECT_EQ(Target->AnnotationCompletions, 3); EXPECT_EQ(Target->UndoBegins, 3); EXPECT_EQ(Target->UndoCompletions, 3);
}

TEST(FTransBufferTests, LaterRecordFailurePreservesOriginalAnnotationForRetry)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotatedAtomicRetry"));
	Target->bAnnotationEnabled = true;
	int OpaqueValue = 0;
	auto Failure = std::make_unique<FTestCustomChange>(OpaqueValue, 0, 1); auto* Fault = Failure.get();
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Annotated atomic retry"});
		ASSERT_TRUE(ApplyEditorChange(*Buffer, std::move(Failure)));
		ASSERT_TRUE(Target->Modify()); Target->Value = 7; Target->SupplementalValue = 3;
	}
	Target->SupplementalValue = 11; Fault->bFailUndo = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Target->Value, 7); EXPECT_EQ(Target->SupplementalValue, 11);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::Idle); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	Fault->bFailUndo = false;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SupplementalValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 7); EXPECT_EQ(Target->SupplementalValue, 11);
}

TEST(FTransBufferTests, WholeAnnotationKeepsFirstStateThroughRepeatedCapture)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "WholeAnnotation"));
	Target->bAnnotationEnabled = true; Target->SupplementalValue = 1;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Whole annotation capture"});
		auto Initial = FTransactionObjectRecord::CaptureWhole(*Target);
		ASSERT_TRUE(Initial); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Initial)));
		Target->Value = 2; Target->SupplementalValue = 7;
		auto Latest = FTransactionObjectRecord::CaptureWhole(*Target);
		ASSERT_TRUE(Latest); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Latest)));
		ASSERT_TRUE(Target->Modify());
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SupplementalValue, 1);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 2); EXPECT_EQ(Target->SupplementalValue, 7);
}

TEST(FEngineTransactionAnnotationTests, WholeRecordRestoresNativeRegistration)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("FocusedNativeActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	auto* Member = Root->GetClass()->FindPropertyByName("CreationMethod"); ASSERT_NE(Member, nullptr);
	FPropertyValueSnapshotPayload Before; ASSERT_TRUE(CapturePropertyValuePayload(Member, Root, 0, Before, true));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Focused native record"});
		auto Record = FTransactionObjectRecord::CaptureWhole(*Root);
		ASSERT_TRUE(Record); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Record)));
		Root->UnregisterComponent();
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_TRUE(Root->IsRegistered()); EXPECT_EQ(Root->Registered, 2u);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_FALSE(Root->IsRegistered()); EXPECT_EQ(Root->Registered, 2u);
}

TEST(FEngineTransactionAnnotationTests, DefaultComponentAnnotationIsImplicitAndRestoresDefaultNativeState)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DSceneComponent> Component(NewObject<DSceneComponent>(nullptr, "DefaultComponentAnnotation"));
	Component->SetFlags(EObjectFlags::Transactional);
	EXPECT_FALSE(Component->FindOrCreateTransactionAnnotation());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Default native snapshot"}); ASSERT_TRUE(Component->Modify());
		Component->SetComponentTickEnabled(true);
	}
	ASSERT_TRUE(Component->FindOrCreateTransactionAnnotation());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_FALSE(Component->IsComponentTickEnabled()); EXPECT_FALSE(Component->FindOrCreateTransactionAnnotation());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Component->IsComponentTickEnabled());
}

TEST(FEngineTransactionAnnotationTests, ExplicitAnnotationArchiveRoundTripsNativeStateWithoutDuplicatingCreation)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("AnnotationArchiveActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	FActorTransactionAnnotation Original(*Actor);
	FByteBuffer Bytes; FObjectMemoryWriter Writer(Bytes, EArchivePurpose::Transaction); Original.Serialize(Writer);
	ASSERT_FALSE(Writer.IsError()) << Writer.GetError();
	Actor->SetActorTickEnabled(true); Root->UnregisterComponent();
	FActorTransactionAnnotation Restored; FObjectMemoryReader Reader(Bytes, EArchivePurpose::Transaction); Restored.Serialize(Reader);
	ASSERT_FALSE(Reader.IsError()) << Reader.GetError(); EXPECT_EQ(Reader.GetRemainingPayloadBytes(), 0u);
	ASSERT_TRUE(Restored.Restore(*Actor)); EXPECT_FALSE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Root->IsRegistered());
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u);
}

TEST(FEngineTransactionAnnotationTests, ActorAndRootRestoreNativeLifecycleOncePerOperation)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("NativeAnnotationActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent()); ASSERT_NE(Root, nullptr);
	ASSERT_EQ(Root->Created, 1u); ASSERT_EQ(Root->Registered, 1u);
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Native annotation lifecycle"}); ASSERT_TRUE(Actor->Modify());
		Actor->SetActorTickEnabled(true); Root->SetComponentTickEnabled(true); Root->UnregisterComponent();
	}
	const auto Constructions = Actor->ConstructionCalls;
	Root->bCollectOnRegister = true;
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_FALSE(Actor->IsActorTickEnabled()); EXPECT_FALSE(Root->IsComponentTickEnabled()); EXPECT_TRUE(Root->IsRegistered());
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u); EXPECT_EQ(Root->Initialized, 1u);
	EXPECT_EQ(Actor->ConstructionCalls, Constructions + 1);
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_TRUE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Root->IsComponentTickEnabled()); EXPECT_FALSE(Root->IsRegistered());
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u); EXPECT_EQ(Root->Unregistered, 2u); EXPECT_EQ(Root->Uninitialized, 0u);
	EXPECT_EQ(Actor->ConstructionCalls, Constructions + 2);
}

TEST(FEngineTransactionAnnotationTests, GeneratedRootRecreatesWithoutRetainingOldGenerationAndRestoresExternalAttachment)
{
	using namespace Durin; using namespace Durin::Editor;
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
		FScopedTransaction Scope(Buffer.Get(), {"test", "Generated root state"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	EXPECT_EQ(Actor->GetRootComponent(), nullptr); CollectGarbage(); EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	Actor->bCollectDuringConstruction = true;
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	auto* Restored = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent()); ASSERT_NE(Restored, nullptr);
	EXPECT_NE(FObjectKey(Restored), OldKey); EXPECT_EQ(Restored->GetWorldLocation(), BeforeWorld); EXPECT_EQ(Restored->CreatedLocation, FVector3(0.0));
	EXPECT_EQ(Restored->Created, 1u); EXPECT_EQ(Restored->Registered, 1u); EXPECT_EQ(Restored->Initialized, 1u);
	EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), Restored); EXPECT_EQ(Other->GetRootComponent()->GetWorldLocation(), ChildWorld);
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_EQ(Actor->GetRootComponent(), nullptr);
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), Actor->GetRootComponent());
	EXPECT_EQ(Other->GetRootComponent()->GetWorldLocation(), ChildWorld);
}

TEST(FEngineTransactionAnnotationTests, SerializedChildEditsAndGeneratedRootReconstructionPreserveBothPoses)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("MixedRootActor");
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("MixedRootChild");
	ASSERT_NE(Actor, nullptr); ASSERT_NE(Other, nullptr);
	auto* Root = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_TRUE(Actor->SetRootComponent(Root)); Root->SetRelativeLocation({5.0, 0.0, 0.0});
	auto* Child = Other->GetRootComponent(); ASSERT_TRUE(Child->AttachToComponent(Root)); Child->SetRelativeLocation({2.0, 0.0, 0.0});
	const auto Before = Child->GetWorldLocation();
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Serialized child and root reconstruction"});
		ASSERT_TRUE(Actor->Modify()); ASSERT_TRUE(Child->Modify()); Child->SetRelativeLocation({3.0, 0.0, 0.0});
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	const auto After = Child->GetWorldLocation(); EXPECT_EQ(After, FVector3(8.0, 0.0, 0.0));
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult()); EXPECT_EQ(Child->GetWorldLocation(), Before);
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult()); EXPECT_EQ(Child->GetWorldLocation(), After);
}

TEST(FEngineTransactionAnnotationTests, SwitchingGeneratedAndAuthoredRootsPreservesBothHistorySides)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("RootKindSwitchActor"); ASSERT_NE(Actor, nullptr);
	auto* NativeRoot = Actor->GetRootComponent();
	auto* GeneratedRoot = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(GeneratedRoot, nullptr); ASSERT_TRUE(Actor->SetRootComponent(GeneratedRoot));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Root kind switch"}); ASSERT_TRUE(Actor->Modify());
		ASSERT_TRUE(Actor->SetRootComponent(NativeRoot));
	}
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult()); EXPECT_EQ(Actor->GetRootComponent(), Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	EXPECT_NE(Actor->GetRootComponent(), GeneratedRoot); EXPECT_TRUE(GeneratedRoot->IsPendingKill());
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult()); EXPECT_EQ(Actor->GetRootComponent(), NativeRoot);
}

TEST(FEngineTransactionAnnotationTests, FailedReconstructionCleansNewOutputAndRequiresRecovery)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("FailedReconstructionActor"); ASSERT_NE(Actor, nullptr);
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Reconstruction failure"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 2; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	const auto Before = Actor->GetComponentsSnapshot(); Actor->bFailConstruction = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_TRUE(std::ranges::none_of(Actor->GetComponents(), [](const auto& Component) { return Component->GetCreationMethod() == EComponentCreationMethod::Generated; }));
	for (const auto& Component : Before)
		if (Component->GetCreationMethod() == EComponentCreationMethod::Generated) EXPECT_TRUE(Component->IsPendingKill());
	Actor->bFailConstruction = false;
	ASSERT_TRUE(Actor->RerunConstructionScripts()); // Failure must release the native construction guard.
	EXPECT_EQ(Actor->GetComponents().size(), 2u);
}

TEST(FEngineTransactionAnnotationTests, ExpiredAnnotationParticipantRejectsBeforeSerializedMutation)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("AnnotationExpiryActor");
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("AnnotationExpiryChild");
	ASSERT_NE(Actor, nullptr); ASSERT_NE(Other, nullptr);
	auto* Generated = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_TRUE(Actor->SetRootComponent(Generated)); ASSERT_TRUE(Other->GetRootComponent()->AttachToComponent(Generated));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Expired annotation participant"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	MarkAsGarbage(Other->GetRootComponent());
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Actor->GeneratedCount, 0); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_NE(Buffer->GetState(), ETransactorState::RecoveryRequired);
}

TEST(FEngineTransactionAnnotationTests, RetirementInsideRegistrationCallbackRequiresRecoveryWithoutReinitializingGarbage)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("CallbackRetirementActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Retiring callback"}); ASSERT_TRUE(Actor->Modify()); Root->UnregisterComponent();
	}
	const auto Initializations = Root->Initialized; Root->bDestroyOnRegister = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_TRUE(Root->IsGarbage()); EXPECT_EQ(Root->Initialized, Initializations); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
}

TEST(FEngineTransactionAnnotationTests, GeneratedSceneOverridesReplayWithoutAnAuthoredPropertyRecord)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedPoseReplay"); ASSERT_NE(Actor, nullptr);
	auto* Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); Scene->SetRelativeLocation({9.0, 0.0, 0.0});
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Generated scene pose"}); ASSERT_TRUE(Actor->Modify());
		Scene->SetRelativeLocation({12.0, 0.0, 0.0}); ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), FVector3(9.0, 0.0, 0.0));
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), FVector3(12.0, 0.0, 0.0));
	Actor->ConstructionLocation = FVector3(12.0, 0.0, 0.0);
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), *Actor->ConstructionLocation);
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	Actor->ConstructionLocation = FVector3(4.0, 0.0, 0.0);
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), *Actor->ConstructionLocation);
}

TEST(FEngineTransactionAnnotationTests, EmptyActorAnnotationIsOmittedAndDefaultTickRestoresOnUndo)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<AActor>("SparseActorAnnotation"); ASSERT_NE(Actor, nullptr);
	ASSERT_TRUE(Actor->GetPrimaryActorTick().IsTickFunctionRegistered());
	EXPECT_FALSE(Actor->FindOrCreateTransactionAnnotation());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Actor tick state"}); ASSERT_TRUE(Actor->Modify());
		Actor->SetActorTickEnabled(true);
		Actor->GetPrimaryActorTick().UnregisterTickFunction();
	}
	EXPECT_TRUE(Actor->FindOrCreateTransactionAnnotation());
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_FALSE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Actor->GetPrimaryActorTick().IsTickFunctionRegistered());
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_TRUE(Actor->IsActorTickEnabled()); EXPECT_FALSE(Actor->GetPrimaryActorTick().IsTickFunctionRegistered());
}

TEST(FEngineTransactionAnnotationTests, InnerRegistrationFailureRequiresRecoveryAndReleasesActorSuspension)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("InnerCallbackFailure");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Inner registration failure"}); ASSERT_TRUE(Actor->Modify());
		Root->UnregisterComponent();
	}
	Root->bThrowOnRegister = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	Root->bThrowOnRegister = false;
	const auto Before = Actor->ConstructionCalls;
	ASSERT_TRUE(Actor->RerunConstructionScripts()); EXPECT_EQ(Actor->ConstructionCalls, Before + 1);
}

TEST(FEngineTransactionAnnotationTests, AttachmentReverseLinksAreSettledBeforeHistoricalRegistrationCallback)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackChild");
	auto* First = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackFirst");
	auto* Second = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackSecond");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	ASSERT_TRUE(Root->AttachToComponent(First->GetRootComponent()));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Attachment callback state"}); ASSERT_TRUE(Root->Modify());
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
	EXPECT_THROW(Actor->PostEditUndo(std::make_shared<FComponentTransactionAnnotation>(*Actor->GetRootComponent())), std::runtime_error);
	const auto Before = Actor->ConstructionCalls;
	ASSERT_TRUE(Actor->RerunConstructionScripts()); EXPECT_EQ(Actor->ConstructionCalls, Before + 1);
}

TEST(FEngineTransactionAnnotationTests, RegisteredDerivedInstanceDataRoundTripsAndCopiesWithoutCentralTypeBranches)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("TypedCacheActor"); ASSERT_NE(Actor, nullptr);
	auto* Component = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Component, nullptr); Component->bTypedInstanceData = true;
	FComponentInstanceDataCache Original(*Actor);
	FByteBuffer Bytes; FObjectMemoryWriter Writer(Bytes, EArchivePurpose::Transaction); Original.Serialize(Writer);
	ASSERT_FALSE(Writer.IsError()) << Writer.GetError();
	FComponentInstanceDataCache Restored; FObjectMemoryReader Reader(Bytes, EArchivePurpose::Transaction); Restored.Serialize(Reader);
	ASSERT_FALSE(Reader.IsError()) << Reader.GetError(); EXPECT_EQ(Reader.GetRemainingPayloadBytes(), 0u);
	FTypedSceneInstanceData::AppliedValue = 0;
	ASSERT_TRUE(Restored.ApplyToActor(*Actor, ECacheApplyPhase::PostUserConstructionScript));
	EXPECT_EQ(FTypedSceneInstanceData::AppliedValue, 42);
	FObjectReplacementMap Map;
	auto Data = Component->GetComponentInstanceData(); auto Copy = Data->CreateReplacement(Map);
	ASSERT_TRUE(Copy) << Copy.error(); ASSERT_NE(Copy->get(), Data.get());
	auto* TypedCopy = dynamic_cast<FTypedSceneInstanceData*>(Copy->get()); ASSERT_NE(TypedCopy, nullptr);
	EXPECT_EQ(TypedCopy->DomainValue, 42); TypedCopy->DomainValue = 91;
	EXPECT_EQ(static_cast<FTypedSceneInstanceData*>(Data.get())->DomainValue, 42);
	const auto Destroyed = FTypedSceneInstanceData::Destroyed;
	{ auto Temporary = Data->GetDataType().Construct(); EXPECT_NE(Temporary, nullptr); }
	EXPECT_EQ(FTypedSceneInstanceData::Destroyed, Destroyed + 1);
}

TEST(FEngineTransactionAnnotationTests, UnregisteredDerivedDataCannotBeSerializedOrSliced)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("UnsupportedCacheActor"); ASSERT_NE(Actor, nullptr);
	auto* Component = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Component, nullptr); Component->bUnsupportedInstanceData = true;
	FComponentInstanceDataCache Cache(*Actor);
	FByteBuffer Bytes; FObjectMemoryWriter Writer(Bytes, EArchivePurpose::Transaction); Cache.Serialize(Writer);
	EXPECT_TRUE(Writer.IsError()); FObjectReplacementMap Map;
	EXPECT_FALSE(Component->GetComponentInstanceData()->CreateReplacement(Map));
	EXPECT_EQ(FActorComponentInstanceData::FindDataType("Tests.UnknownInstanceData"), nullptr);
	FByteBuffer UnknownBytes; FObjectMemoryWriter UnknownWriter(UnknownBytes, EArchivePurpose::Transaction);
	auto ActorKey = FObjectKey(Actor); ActorKey.SerializeForSnapshot(UnknownWriter);
	uint64 Count = 1; std::string UnknownName = "Tests.UnknownInstanceData"; UnknownWriter << Count << UnknownName;
	ASSERT_FALSE(UnknownWriter.IsError());
	FComponentInstanceDataCache UnknownCache; FObjectMemoryReader UnknownReader(UnknownBytes, EArchivePurpose::Transaction);
	UnknownCache.Serialize(UnknownReader); EXPECT_TRUE(UnknownReader.IsError());
}
