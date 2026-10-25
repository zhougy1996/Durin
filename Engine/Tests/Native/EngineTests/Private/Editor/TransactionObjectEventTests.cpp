#include "Editor/TransactionRecordTestSupport.h"

using namespace Durin::Tests::ObjectState;

TEST(FTransactionObjectEventTests, OrdinaryEditAdmissionKeepsDomainParticipantsAndRejectsForeignScopes)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer, Other;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "MixedDomainFields"));
	EXPECT_FALSE(RecordObjectForEdit(*Buffer, *Target));
	int DomainValue = 1;
	ASSERT_EQ(Buffer->Begin("test", "Domain and ordinary fields"), 0);
	ASSERT_TRUE(GUndo->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(DomainValue, 0, 1)));
	ASSERT_TRUE(RecordObjectForEdit(*Buffer, *Target));
	const auto Saves = Target->SerializationSaves;
	ASSERT_TRUE(RecordObjectForEdit(*Buffer, *Target)); EXPECT_EQ(Target->SerializationSaves, Saves);
	EXPECT_FALSE(RecordObjectForEdit(*Other, *Target)); EXPECT_EQ(Target->SerializationSaves, Saves);
	Target->Value = 9;
	EXPECT_EQ(static_cast<FTransaction*>(GUndo)->GetRecordCount(), 2u);
	EXPECT_EQ(Buffer->End(), 1);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(DomainValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 9); EXPECT_EQ(DomainValue, 1);
}

TEST(FTransactionObjectEventTests, FailedReplayDoesNotPublishAnExternalReferenceIntoHistory)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "RetryHistory"));
	int DomainValue = 1;
	auto Change = std::make_unique<FTestCustomChange>(DomainValue, 0, 1);
	auto* Control = Change.get();
	Control->bFailUndo = true;
	ASSERT_EQ(Buffer->Begin("test", "Failed replay history"), 0);
	ASSERT_TRUE(GUndo->StoreUndo(Target.Get(), std::move(Change)));
	ASSERT_TRUE(RecordObjectForEdit(*Buffer, *Target));
	Target->Value = 9;
	EXPECT_EQ(Buffer->End(), 1);
	auto* External = NewObject<DTransactionRecordParticipant>(nullptr, "ExternalReference");
	Target->Hard = External;
	EXPECT_FALSE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 9);
	EXPECT_EQ(Target->Hard.Get(), External);
	EXPECT_TRUE(Buffer->CanUndo());
	Target->Hard = nullptr;
	MarkObjectHierarchyAsGarbage(External);
	CollectGarbage();
	Control->bFailUndo = false;
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_EQ(Target->Value, 0);
	EXPECT_EQ(DomainValue, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 9);
	EXPECT_EQ(Target->Hard.Get(), nullptr);
	EXPECT_EQ(DomainValue, 1);
}

TEST(FTransactionObjectEventTests, ChangedTransactionRetainsAnUnchangedObjectBaseline)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "UnchangedBaseline"));
	int DomainValue = 1;
	ASSERT_EQ(Buffer->Begin("test", "Keep complete transaction"), 0);
	ASSERT_TRUE(Target->Modify());
	ASSERT_TRUE(GUndo->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(DomainValue, 0, 1)));
	EXPECT_EQ(Buffer->End(), 1);
	ASSERT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 2u);
	Target->Value = 7;
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	EXPECT_EQ(DomainValue, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 7);
	EXPECT_EQ(DomainValue, 1);
}

TEST(FTransactionObjectEventTests, RepeatedSnapshotsRetainOneEntryAndReleaseObservationStorageAtEnd)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "BoundedSnapshots"));
	Target->Numbers.assign(1024u, 0);
	ASSERT_EQ(Buffer->Begin("test", "Bounded capture"), 0); ASSERT_TRUE(Target->Modify());
	Target->Value = 1; Target->Numbers[0] = 1;
	EXPECT_EQ(Buffer->End(), 1);
	const auto OrdinaryBytes = Buffer->GetUndoSize(); ASSERT_GT(OrdinaryBytes, 0u);
	ASSERT_TRUE(Buffer->TryReset()); Target->Value = 0; Target->Numbers[0] = 0;
	const auto Saves = Target->SerializationSaves;
	ASSERT_EQ(Buffer->Begin("test", "Bounded capture"), 0); ASSERT_TRUE(Target->Modify());
	auto* Transaction = static_cast<FTransaction*>(GUndo);
	size_t PeakBytes = 0;
	for (int32 Index = 1; Index <= 32; ++Index)
	{
		Target->Value = Index; Target->Numbers[0] = Index;
		ASSERT_TRUE(GUndo->SnapshotObject(Target.Get()));
		size_t CurrentBytes = 0; ASSERT_TRUE(Transaction->TryGetOwnedSize(CurrentBytes));
		if (Index == 1) PeakBytes = CurrentBytes;
		else EXPECT_EQ(CurrentBytes, PeakBytes);
		EXPECT_EQ(Transaction->GetRecordCount(), 1u); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
		EXPECT_EQ(Target->SerializationSaves, Saves + 1 + Index);
	}
	EXPECT_EQ(Buffer->End(), 1);
	const auto FinalBytes = Buffer->GetUndoSize();
	EXPECT_EQ(FinalBytes, OrdinaryBytes); ASSERT_LT(FinalBytes, PeakBytes);
	EXPECT_GE(PeakBytes - FinalBytes, Target->Numbers.size() * sizeof(int32));
	ASSERT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	for (uint32 Index = 0; Index < 8; ++Index)
	{
		ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->Numbers[0], 0);
		EXPECT_EQ(Buffer->GetUndoSize(), FinalBytes);
		ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 32); EXPECT_EQ(Target->Numbers[0], 32);
		EXPECT_EQ(Buffer->GetUndoSize(), FinalBytes); EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	}
	ASSERT_TRUE(Buffer->TryReset()); EXPECT_EQ(Buffer->GetUndoSize(), 0u);
	RecordProperty("ordinary_history_bytes", std::to_string(OrdinaryBytes));
	RecordProperty("snapshot_peak_bytes", std::to_string(PeakBytes));
	RecordProperty("final_history_bytes", std::to_string(FinalBytes));
	RecordProperty("snapshots", "32");
}

TEST(FTransactionObjectEventTests, SynchronousEventsKeepContextAndRejectRecordingReentry)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SynchronousObjectEvents"));
	std::vector<FTransactionObjectEvent> Events; std::vector<std::string> Trace;
	Target->Trace = [&](std::string_view Value) { Trace.emplace_back(Value); };
	ITransaction* Recording = nullptr;
	Target->TransactionObserver = [&](const FTransactionObjectEvent& Event) {
		Events.push_back(Event); Trace.push_back("event");
		EXPECT_NE(Event.OperationId, 0u); EXPECT_NE(Event.TransactionId, 0u);
		if (Event.Type == ETransactionObjectEventType::Finalized)
		{
			EXPECT_EQ(GUndo, Recording); EXPECT_EQ(GUndo->GetContext().OperationId, Event.OperationId);
			EXPECT_EQ(Buffer->Begin("reentry", "reentry"), -1); EXPECT_EQ(Buffer->End(), 0);
			Buffer->Cancel(); EXPECT_EQ(GUndo, Recording);
			const auto Saves = Target->SerializationSaves; (void)Target->Modify(); EXPECT_EQ(Target->SerializationSaves, Saves);
			EXPECT_FALSE(RecordObjectForEdit(*Buffer, *Target)); EXPECT_EQ(Target->SerializationSaves, Saves);
			EXPECT_FALSE(Recording->EndOperation()); EXPECT_FALSE(Recording->Finalize());
			EXPECT_FALSE(Recording->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 0, 99)));
			EXPECT_EQ(Buffer->GetQueueLength(), 0u);
		}
	};
	ASSERT_EQ(Buffer->Begin("test", "Events"), 0); Recording = GUndo;
	ASSERT_TRUE(Target->Modify()); Target->Value = 4; Target->NativeValue = 9;
	ASSERT_EQ(Buffer->Begin("nested", "Nested"), 1); EXPECT_EQ(Buffer->End(), 2); EXPECT_TRUE(Events.empty());
	EXPECT_EQ(Buffer->End(), 1); EXPECT_EQ(GUndo, nullptr); ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events[0].Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Events[0].Change.ChangedProperties, std::vector<FName>{"Value"}); EXPECT_TRUE(Events[0].Change.bNonPropertyDataChanged);
	Trace.clear(); ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Trace, (std::vector<std::string>{"pre", "post", "event"}));
	ASSERT_EQ(Events.size(), 2u); EXPECT_EQ(Events[1].Operation, ETransactionOperation::Undo);
	EXPECT_NE(Events[0].OperationId, Events[1].OperationId);
	ASSERT_TRUE(Buffer->Redo()); ASSERT_EQ(Events.size(), 3u);
	EXPECT_EQ(Events[2].Operation, ETransactionOperation::Redo); EXPECT_NE(Events[1].OperationId, Events[2].OperationId);
	EXPECT_EQ(Events[0].TransactionId, Events[2].TransactionId);
}

TEST(FTransactionObjectEventTests, CancelNoOpAndCustomOnlyDoNotInventObjectEvents)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SilentObjectEvents"));
	uint32 Events = 0; Target->TransactionObserver = [&](const auto&) { ++Events; };
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "No op"}); ASSERT_TRUE(Target->Modify()); }
	EXPECT_EQ(Events, 0u); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Cancel"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; Scope.Cancel(); }
	EXPECT_EQ(Events, 0u);
	ASSERT_EQ(Buffer->Begin("test", "Custom observation"), 0);
	Target->Value = 2; ASSERT_TRUE(Buffer->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 1, 2)));
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); EXPECT_EQ(Events, 0u); EXPECT_EQ(Buffer->End(), 1);
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Events, 0u); EXPECT_EQ(Target->UndoCompletions, 2);
}

TEST(FTransactionObjectEventTests, CompensatedFailureEmitsNoSuccessfulReplayEvent)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CompensatedObjectEvents"));
	uint32 Events = 0; Target->TransactionObserver = [&](const auto&) { ++Events; };
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Failure"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	ASSERT_EQ(Events, 1u); Target->bFailLoadOnce = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Events, 1u); EXPECT_EQ(Target->Value, 4); EXPECT_TRUE(Buffer->CanUndo());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Events, 2u);
}

TEST(FTransactionObjectEventTests, ObserverExceptionRetainsRecordingAndRequiresRecovery)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingFinalizedEvent"));
	Target->TransactionObserver = [](const auto&) { throw std::runtime_error("Injected object event failure."); };
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Observer failure"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired); EXPECT_EQ(Buffer->GetLastResult().Code, ETransactorResultCode::RecoveryRequired);
	EXPECT_FALSE(Buffer->CanTransact()); EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_EQ(GUndo, nullptr); EXPECT_EQ(Target->Value, 4);
	TWeakObjectPtr<DTransactionRecordParticipant> Retained(Target.Get()); Target.Reset(); CollectGarbage(); EXPECT_TRUE(Retained.IsValid());
}

TEST(FTransactionObjectEventTests, ReplayObserverExceptionKeepsCursorAndCompletedNativeState)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingReplayEvent"));
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Replay observer"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	const auto Id = Buffer->GetUndoId();
	Target->TransactionObserver = [](const auto&) { throw std::runtime_error("Injected replay object event failure."); };
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_EQ(Buffer->GetUndoId(), Id); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->UndoCompletions, 1);
}

TEST(FTransactionObjectEventTests, SnapshotsAccumulateSelectionWithoutReplacingUndoBaseline)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SelectedSnapshots"));
	std::vector<FTransactionObjectEvent> Events;
	Target->TransactionObserver = [&](const auto& Event) {
		Events.push_back(Event); EXPECT_EQ(GUndo->GetContext().OperationId, Event.OperationId);
		EXPECT_FALSE(GUndo->SnapshotObject(Target.Get())); EXPECT_EQ(Buffer->Begin("reentry", "snapshot"), -1);
	};
	ASSERT_EQ(Buffer->Begin("test", "Snapshot selection"), 0);
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); EXPECT_TRUE(Events.empty()); ASSERT_TRUE(Target->Modify());
	const auto Operation = GUndo->GetContext().OperationId;
	const std::array<const FProperty*, 1> Value{Property("Value")}, Session{Property("SessionValue")};
	Target->Value = 1; Target->SessionValue = 2;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Value)); ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events.back().Change.ChangedProperties, std::vector<FName>{"Value"});
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Session)); EXPECT_EQ(Events.size(), 1u);
	Target->Value = 3; Target->SessionValue = 4;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Session)); ASSERT_EQ(Events.size(), 2u);
	EXPECT_TRUE(std::ranges::contains(Events.back().Change.ChangedProperties, FName("Value")));
	EXPECT_TRUE(std::ranges::contains(Events.back().Change.ChangedProperties, FName("SessionValue")));
	Target->Numbers = {1}; ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); ASSERT_EQ(Events.size(), 3u);
	Target->Numbers = {2}; ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Value)); ASSERT_EQ(Events.size(), 4u);
	EXPECT_EQ(Events.back().Change.ChangedProperties, std::vector<FName>{"Numbers"});
	EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_EQ(GUndo->GetContext().OperationId, Operation);
	Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	EXPECT_EQ(Buffer->End(), 1); ASSERT_EQ(Events.size(), 5u); EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SessionValue, 0); EXPECT_TRUE(Target->Numbers.empty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 3); EXPECT_EQ(Target->SessionValue, 4); EXPECT_EQ(Target->Numbers, std::vector<int32>{2});
}

TEST(FTransactionObjectEventTests, WholeObjectSnapshotsObserveNativeDataAndKeepOriginalUndo)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "WholeSnapshots"));
	std::vector<FTransactionObjectEvent> Events; Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Whole snapshot"), 0);
	auto Record = FTransactionObjectRecord::CaptureWhole(*Target);
	ASSERT_TRUE(Record); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Record)));
	Target->Value = 2; Target->NativeValue = 8;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); ASSERT_EQ(Events.size(), 1u);
	EXPECT_GT(Target->SerializationSaves, 0); EXPECT_TRUE(Events.back().Change.bNonPropertyDataChanged);
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); EXPECT_EQ(Events.size(), 1u);
	ASSERT_TRUE(Target->Modify()); Target->Value = 4; Target->NativeValue = 9;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Buffer->End(), 1); ASSERT_EQ(Events.size(), 3u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->NativeValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 4); EXPECT_EQ(Target->NativeValue, 9);
}

TEST(FTransactionObjectEventTests, SnapshotObserverFailureClosesRecordingAndRetainsRecoveryGraph)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingSnapshotEvent"));
	ASSERT_EQ(Buffer->Begin("test", "Snapshot failure"), 0); ASSERT_TRUE(Target->Modify()); Target->Value = 1;
	Target->TransactionObserver = [](const auto&) { throw std::runtime_error("Injected snapshot observer failure."); };
	EXPECT_FALSE(GUndo->SnapshotObject(Target.Get())); EXPECT_EQ(GUndo, nullptr);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired); EXPECT_FALSE(Buffer->IsActive());
	TWeakObjectPtr<DTransactionRecordParticipant> Retained(Target.Get()); Target.Reset(); CollectGarbage(); EXPECT_TRUE(Retained.IsValid());
}

TEST(FTransactionObjectEventTests, MultipleTargetsCompleteBeforeRecordAndAnnotationEvents)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationEventOwner"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationEventPeer"));
	First->bAnnotationEnabled = First->bReportAnnotationChanges = true; First->AnnotationReference = Second.Get();
	std::vector<std::string> Trace; std::vector<FTransactionObjectEvent> PeerEvents;
	First->Trace = [&](std::string_view Phase) { Trace.push_back(std::string(Phase) + "1"); };
	Second->Trace = [&](std::string_view Phase) { Trace.push_back(std::string(Phase) + "2"); };
	First->TransactionObserver = [&](const auto&) { Trace.push_back("event1"); };
	Second->TransactionObserver = [&](const auto& Event) { Trace.push_back("event2"); PeerEvents.push_back(Event); };
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Record and annotation events"}); ASSERT_TRUE(First->Modify()); ASSERT_TRUE(Second->Modify()); First->SupplementalValue = 3; Second->Value = 7; }
	ASSERT_EQ(PeerEvents.size(), 2u); EXPECT_EQ(Trace, (std::vector<std::string>{"event1", "event2", "event2"}));
	EXPECT_TRUE(PeerEvents[0].Change.bNonPropertyDataChanged); EXPECT_EQ(PeerEvents[0].CreatedBy, ETransactionObjectChangeCreatedBy::TransactionAnnotation);
	EXPECT_FALSE(PeerEvents.back().Change.bNonPropertyDataChanged); EXPECT_EQ(PeerEvents.back().Change.ChangedProperties, std::vector<FName>{"Value"});
	EXPECT_EQ(PeerEvents.back().CreatedBy, ETransactionObjectChangeCreatedBy::TransactionRecord);
	EXPECT_FALSE(PeerEvents.back().Annotation); EXPECT_EQ(PeerEvents.back().Change.OriginalName, Second->GetFName());
	Trace.clear(); ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Trace, (std::vector<std::string>{"pre1", "pre2", "post1", "post2", "event1", "event2", "event2"}));
	ASSERT_EQ(PeerEvents.size(), 4u); EXPECT_TRUE(PeerEvents[2].Change.bNonPropertyDataChanged);
	EXPECT_EQ(First->SupplementalValue, 0); EXPECT_EQ(Second->Value, 0);
}

TEST(FTransactionObjectEventTests, FinalizedAnnotationComparesTheLastSnapshotAfterReturningToOriginal)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Owner(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationSnapshotOwner"));
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationSnapshotPeer"));
	Owner->bAnnotationEnabled = Owner->bReportAnnotationChanges = true; Owner->AnnotationReference = Peer.Get();
	std::vector<FTransactionObjectEvent> Events; Peer->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Annotation snapshot closure"), 0); ASSERT_TRUE(Owner->Modify());
	Owner->SupplementalValue = 3; ASSERT_TRUE(GUndo->SnapshotObject(Owner.Get())); ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Snapshot);
	Owner->SupplementalValue = 0; EXPECT_EQ(Buffer->End(), 1); ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Events.back().CreatedBy, ETransactionObjectChangeCreatedBy::TransactionAnnotation);
	EXPECT_TRUE(Events.back().Change.bNonPropertyDataChanged); EXPECT_FALSE(Events.back().Annotation);
}

TEST(FTransactionObjectEventTests, FinalizedClosesLastSnapshotEvenWhenHistoryReturnsToNoOp)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SnapshotClosureNoOp"));
	std::vector<FTransactionObjectEvent> Events;
	Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Snapshot closure"), 0); ASSERT_TRUE(Target->Modify());
	const auto Context = GUndo->GetContext();
	Target->Value = 4; Target->NativeValue = 9;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); ASSERT_EQ(Events.size(), 1u);
	Target->Value = 0; Target->NativeValue = 0;
	EXPECT_EQ(Buffer->End(), 1); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	ASSERT_EQ(Events.size(), 2u);
	const auto& Final = Events.back();
	EXPECT_EQ(Final.Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Final.CreatedBy, ETransactionObjectChangeCreatedBy::TransactionRecord);
	EXPECT_EQ(Final.TransactionId, Context.TransactionId); EXPECT_EQ(Final.OperationId, Context.OperationId);
	EXPECT_EQ(Final.Change.OriginalObject, FObjectKey(Target.Get()));
	EXPECT_EQ(Final.Change.ChangedProperties, (std::vector<FName>{"Value"}));
	EXPECT_TRUE(Final.Change.bNonPropertyDataChanged);
}

TEST(FTransactionObjectEventTests, SelectedSnapshotClosureDoesNotReportUnobservedNetZeroFields)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SelectedSnapshotClosure"));
	std::vector<FTransactionObjectEvent> Events;
	Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Selected closure"), 0); ASSERT_TRUE(Target->Modify());
	Target->Value = 4; Target->SessionValue = 9;
	const std::array<const FProperty*, 1> Selected{Property("Value")};
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Selected)); ASSERT_EQ(Events.size(), 1u);
	Target->Value = 0; Target->SessionValue = 0;
	EXPECT_EQ(Buffer->End(), 1); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events.back().Change.ChangedProperties, (std::vector<FName>{"Value"}));
	EXPECT_FALSE(Events.back().Change.bNonPropertyDataChanged);
}
