#include "Editor/TransactionRecordTestSupport.h"

using namespace Durin::Tests::ObjectState;

TEST(FTransactionObjectEventTests, MixedCommandAndSnapshotShareParticipantNotifications)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "MixedDomainFields"));
	EXPECT_FALSE(Target->Modify(false));
	int DomainValue = 1;
	std::vector<ETransactionObjectEventType> Events;
	Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event.Type); };
	ASSERT_EQ(Buffer->Begin("test", "Domain and ordinary fields"), 0);
	ASSERT_TRUE(Target->Modify(false));
	ASSERT_TRUE(GUndo->StoreUndo(std::make_unique<FTestCustomChange>(Target.Get(), DomainValue, 0, 1)));
	const auto Saves = Target->SerializationSaves;
	ASSERT_TRUE(Target->Modify(false)); EXPECT_EQ(Target->SerializationSaves, Saves);
	Target->Value = 9;
	EXPECT_EQ(static_cast<FTransaction*>(GUndo)->GetRecordCount(), 2u);
	EXPECT_EQ(Buffer->End(), 1);
	EXPECT_EQ(Events, (std::vector{ETransactionObjectEventType::Finalized}));
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(DomainValue, 0);
	EXPECT_EQ(Events, (std::vector{ETransactionObjectEventType::Finalized, ETransactionObjectEventType::UndoRedo}));
	Events.clear();
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 9); EXPECT_EQ(DomainValue, 1);
	EXPECT_TRUE(Events.empty()); EXPECT_EQ(Target->UndoCompletions, 2);
}

TEST(FTransactionObjectEventTests, ExternalReferenceReplayFaultIsTerminal)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "RetryHistory"));
	int DomainValue = 1;
	auto Change = std::make_unique<FTestCustomChange>(DomainValue, 0, 1);
	auto* Control = Change.get();
	Control->bFailUndo = true;
	ASSERT_EQ(Buffer->Begin("test", "Failed replay history"), 0);
	ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
	ASSERT_TRUE(Target->Modify(false));
	Target->Value = 9;
	EXPECT_EQ(Buffer->End(), 1);
	auto* External = NewObject<DTransactionRecordParticipant>(nullptr, "ExternalReference");
	Target->Hard = External;
	const FObjectKey ExternalKey(External);
	EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
}

TEST(FTransactionObjectEventTests, ChangedTransactionRetainsAnUnchangedObjectBaseline)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "UnchangedBaseline"));
	int DomainValue = 1;
	ASSERT_EQ(Buffer->Begin("test", "Keep complete transaction"), 0);
	ASSERT_TRUE(Target->Modify());
	ASSERT_TRUE(GUndo->StoreUndo(std::make_unique<FTestCustomChange>(Target.Get(), DomainValue, 0, 1)));
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

TEST(FTransactionObjectEventTests, SynchronousEventsKeepContextAndRejectRecordingReentry)
{
	using namespace Durin; using namespace Durin;
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
			EXPECT_FALSE(Recording->EndOperation());
			EXPECT_FALSE(Recording->StoreUndo(std::make_unique<FTestCustomChange>(Target.Get(), Target->Value, 0, 99)));
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
	ASSERT_EQ(Events.size(), 2u); EXPECT_EQ(Events[1].Type, ETransactionObjectEventType::UndoRedo);
	EXPECT_NE(Events[0].OperationId, Events[1].OperationId);
	ASSERT_TRUE(Buffer->Redo()); ASSERT_EQ(Events.size(), 3u);
	EXPECT_EQ(Events[2].Type, ETransactionObjectEventType::UndoRedo); EXPECT_NE(Events[1].OperationId, Events[2].OperationId);
	EXPECT_EQ(Events[0].TransactionId, Events[2].TransactionId);
}

TEST(FTransactionObjectEventTests, RepeatedFinalizeDispatchesRecordedDeltaWithoutRecapturing)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "RepeatedFinalize"));
	FTransaction Transaction(42, {.Description = "Finalize twice"});
	ASSERT_TRUE(Transaction.BeginOperation());
	Target->bAnnotationEnabled = true;
	Transaction.SaveObject(Target.Get());
	Target->Value = 4;
	// The nested call captures the second record while the first is serializing.
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(nullptr, "InnerCapture"));
	Second->bAnnotationEnabled = true;
	Transaction.SaveObject(Second.Get());
	Second->Value = 1;
	bool bReentered = false;
	Target->OnTransactionSave = [&] { if (!std::exchange(bReentered, true)) Transaction.Finalize(); };
	std::vector<bool> SecondAnnotations;
	Second->TransactionObserver = [&](const auto& Event) { SecondAnnotations.push_back(bool(Event.Annotation)); };
	std::vector<FTransactionObjectEvent> Events;
	Target->TransactionObserver = [&](const FTransactionObjectEvent& Event) {
		Events.push_back(Event);
		if (Events.size() == 1) Transaction.Finalize();
	};
	const auto Saves = Target->SerializationSaves;
	Transaction.Finalize();
	ASSERT_EQ(Events.size(), 2u);
	EXPECT_TRUE(Events.front().Annotation);
	EXPECT_EQ(SecondAnnotations, (std::vector<bool>{true, false, false}));
	Target->NativeValue = 15;
	Transaction.Finalize();
	ASSERT_EQ(Events.size(), 3u);
	EXPECT_FALSE(Events[1].Annotation);
	EXPECT_FALSE(Events[2].Annotation);
	for (const auto& Event : Events)
	{
		EXPECT_EQ(Event.Type, ETransactionObjectEventType::Finalized);
		EXPECT_EQ(Event.Change.ChangedProperties, (std::vector<FName>{"Value"}));
		EXPECT_FALSE(Event.Change.bNonPropertyDataChanged);
	}
	EXPECT_EQ(Target->SerializationSaves, Saves + 1);
	EXPECT_TRUE(Transaction.EndOperation());
}

TEST(FTransactionObjectEventTests, CancelNoOpAndCustomOnlyDoNotInventObjectEvents)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SilentObjectEvents"));
	uint32 Events = 0; Target->TransactionObserver = [&](const auto&) { ++Events; };
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "No op"}); ASSERT_TRUE(Target->Modify()); }
	EXPECT_EQ(Events, 0u); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Cancel"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; Scope.Cancel(); }
	EXPECT_EQ(Events, 0u);
	ASSERT_EQ(Buffer->Begin("test", "Custom observation"), 0);
	Target->Value = 2; ASSERT_TRUE(Buffer->StoreUndo(std::make_unique<FTestCustomChange>(Target.Get(), Target->Value, 1, 2)));
	EXPECT_EQ(Events, 0u); EXPECT_EQ(Buffer->End(), 1);
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Events, 0u); EXPECT_EQ(Target->UndoCompletions, 2);
}

TEST(FTransactionObjectEventTests, LoadFaultTerminatesUndo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CompensatedObjectEvents"));
	uint32 Events = 0; Target->TransactionObserver = [&](const auto&) { ++Events; };
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Failure"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	ASSERT_EQ(Events, 1u); Target->bFailLoadOnce = true;
	EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
}

TEST(FTransactionObjectEventTests, FinalizedObserverFaultIsTerminal)
{
	auto Run = [&] {
		using namespace Durin;
		InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
		TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingFinalizedEvent"));
		Target->TransactionObserver = [](const auto&) { throw std::runtime_error("Injected object event failure."); };
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Observer failure"});
		ASSERT_TRUE(Target->Modify());
		Target->Value = 4;
	};
	EXPECT_DEATH(Run(), "Injected object event failure");
}

TEST(FTransactionObjectEventTests, ReplayObserverFaultIsTerminal)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingReplayEvent"));
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Replay observer"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	Target->TransactionObserver = [](const auto&) { throw std::runtime_error("Injected replay object event failure."); };
	EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
}

TEST(FTransactionObjectEventTests, RecordAndAnnotationEventsFollowEachObjectInOperationOrder)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationEventOwner"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(First.Get(), "AnnotationEventPeer"));
	First->bAnnotationEnabled = First->bReportAnnotationChanges = true; First->AnnotationReference = Second.Get();
	std::vector<std::string> Trace; std::vector<FTransactionObjectEvent> PeerEvents;
	First->Trace = [&](std::string_view Phase) { Trace.push_back(std::string(Phase) + "1"); };
	Second->Trace = [&](std::string_view Phase) { Trace.push_back(std::string(Phase) + "2"); };
	First->TransactionObserver = [&](const auto&) { Trace.push_back("event1"); };
	Second->TransactionObserver = [&](const auto& Event) { Trace.push_back("event2"); PeerEvents.push_back(Event); };
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Record and annotation events"}); ASSERT_TRUE(First->Modify()); ASSERT_TRUE(Second->Modify()); First->SupplementalValue = 2; Second->Value = 7;

	}
	ASSERT_EQ(PeerEvents.size(), 2u); EXPECT_EQ(Trace, (std::vector<std::string>{"event1", "event2", "event2"}));
	EXPECT_TRUE(PeerEvents[0].Change.bNonPropertyDataChanged); EXPECT_EQ(PeerEvents[0].CreatedBy, ETransactionObjectChangeCreatedBy::TransactionAnnotation);
	EXPECT_FALSE(PeerEvents.back().Change.bNonPropertyDataChanged); EXPECT_EQ(PeerEvents.back().Change.ChangedProperties, std::vector<FName>{"Value"});
	EXPECT_EQ(PeerEvents.back().CreatedBy, ETransactionObjectChangeCreatedBy::TransactionRecord);
	EXPECT_FALSE(PeerEvents.back().Annotation); EXPECT_EQ(PeerEvents.back().Change.OriginalName, Second->GetFName());
	Trace.clear(); ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Trace, (std::vector<std::string>{"pre2", "pre1", "post2", "event2", "post1", "event1", "event2"}));
	ASSERT_EQ(PeerEvents.size(), 4u); EXPECT_FALSE(PeerEvents[2].Change.bNonPropertyDataChanged); EXPECT_TRUE(PeerEvents[3].Change.bNonPropertyDataChanged);
	EXPECT_EQ(First->SupplementalValue, 0); EXPECT_EQ(Second->Value, 0);
	Trace.clear(); ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Trace, (std::vector<std::string>{"pre1", "pre2", "post1", "event1", "event2", "post2", "event2"}));
}
