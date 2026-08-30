#include "ReflectedPropertyEditingTestSupport.h"

TEST(FReflectedPropertyEditSessionTests, KeepsTargetAliveForTheSession)
{
	InitializeDObjectSystem();
	auto Property = MakeValueProperty();
	FValueContainer Container{4};
	Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PropertyEditTarget"));
	Durin::Editor::FPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.SnapshotProperty = Property.get();
	Target.SnapshotContainer = &Container;
	Target.Path.push_back({Property.get()});

	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Rooted Edit"));
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Object));
	ASSERT_EQ(Session.Cancel(), Durin::Editor::EPropertyEditResult::NoChange);
	Durin::CollectGarbage();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Object));
}

TEST(FReflectedPropertyEditSessionTests, RejectsACollectedTargetBeforeReadingItsSnapshotStorage)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "CollectedPropertyEditTarget");
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	const Durin::Editor::FPropertyEditTarget Target =
		Durin::Editor::FPropertyEditTarget::ForMember(Object, Property);
	Durin::CollectGarbage();

	Durin::Editor::FPropertyEditSession Session;
	std::string Error;
	EXPECT_FALSE(Session.Begin(Target, "Stale Edit", &Error));
	EXPECT_EQ(Error, "The reflected-property edit target is no longer live.");
}

TEST(FReflectedPropertyEditSessionTests, ContinuousCommitCreatesOneUndoRedoTransaction)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "ContinuousTransactionTarget");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(Object);
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 7;
	auto Capture = [&](int32 Value) {
		const int32 Saved = Object->Value;
		Object->Value = Value;
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, Snapshot));
		Object->Value = Saved;
		return Snapshot;
	};
	FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::Editor::FPropertyEditTarget::ForMember(Object, Property), "Edit Value", nullptr,
		Transactions.Get()));

	EXPECT_EQ(Session.Apply(Capture(11)), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Apply(Capture(19)), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_TRUE(Transactions.Get()->CanUndo());
	EXPECT_FALSE(Transactions.Get()->CanRedo());
	EXPECT_EQ(Transactions.Get()->GetUndoDescription(), "Edit Value");
	ASSERT_EQ(Transactions.Get()->ConsumeEvents().size(), 1u);

	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 7);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	EXPECT_TRUE(Transactions.Get()->CanRedo());
	ASSERT_EQ(Object->Changes.size(), 4u);
	EXPECT_EQ(Object->Changes[2].Phase, Durin::EPropertyChangePhase::Committed);
	EXPECT_EQ(Object->Changes[2].Origin, Durin::EPropertyChangeOrigin::Edit);
	EXPECT_EQ(Object->Changes[3].Phase, Durin::EPropertyChangePhase::Committed);
	EXPECT_EQ(Object->Changes[3].Origin, Durin::EPropertyChangeOrigin::Undo);

	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 19);
	EXPECT_TRUE(Transactions.Get()->CanUndo());
	EXPECT_FALSE(Transactions.Get()->CanRedo());
	ASSERT_EQ(Object->Changes.size(), 5u);
	EXPECT_EQ(Object->Changes[4].Origin, Durin::EPropertyChangeOrigin::Redo);
	const std::vector<Durin::Editor::FTransactionEvent> Events = Transactions.Get()->ConsumeEvents();
	ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events[0].Type, Durin::Editor::ETransactionEventType::Undone);
	EXPECT_EQ(Events[1].Type, Durin::Editor::ETransactionEventType::Redone);
}

TEST(FReflectedPropertyEditSessionTests, SynchronizesPackageDirtyStateAtSavedRevision)
{
	Durin::DPackage* Package = MakeReflectedRevisionTestPackage();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(Package, "PropertyEditTarget");
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 7;
	const Durin::Editor::FPropertyEditTarget Target =
		Durin::Editor::FPropertyEditTarget::ForMember(Object, Property);
	Object->Value = 19;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, Proposed));
	Object->Value = 7;
	FTestTransactorOwner Transactions;
	Transactions.Get()->EstablishSavedState(*Package);

	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Saved Value", nullptr, Transactions.Get()));
	ASSERT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 7);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 19);
	EXPECT_TRUE(Package->IsDirty());
	Transactions.Get()->MarkSaved(*Package);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_FALSE(Package->IsDirty());
}

TEST(FReflectedPropertyEditSessionTests, NoOpAndCancelledEditsDoNotCreateTransactions)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "NoOpTransactionTarget");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(Object);
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 3;
	const auto Target = Durin::Editor::FPropertyEditTarget::ForMember(Object, Property);
	Object->Value = 9;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, Proposed));
	Object->Value = 3;
	FTestTransactorOwner Transactions;

	Durin::Editor::FPropertyEditSession NoOpSession;
	ASSERT_TRUE(NoOpSession.Begin(Target, "No-op", nullptr,
		Transactions.Get()));
	EXPECT_EQ(NoOpSession.Commit(), Durin::Editor::EPropertyEditResult::NoChange);

	Durin::Editor::FPropertyEditSession CancelledSession;
	ASSERT_TRUE(CancelledSession.Begin(Target, "Cancelled", nullptr,
		Transactions.Get()));
	ASSERT_EQ(CancelledSession.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(CancelledSession.Cancel(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Object->Value, 3);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	const auto Events = Transactions.Get()->ConsumeEvents();
	ASSERT_EQ(Events.size(), 2u);
	EXPECT_TRUE(std::ranges::all_of(Events, [](const auto& Event) {
		return Event.Type == Durin::Editor::ETransactionEventType::Discarded;
	}));
}

TEST(FReflectedPropertyEditSessionTests, TransactionHistoryKeepsTargetAlive)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "TransactionalPropertyTarget");
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 4;
	const auto Target = Durin::Editor::FPropertyEditTarget::ForMember(Object, Property);
	Object->Value = 12;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, Proposed));
	Object->Value = 4;
	FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Rooted Value", nullptr, Transactions.Get()));
	ASSERT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);

	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Object));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 4);
	Transactions.Get()->Reset();
	Durin::CollectGarbage();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Object));
}

TEST(FReflectedPropertyEditSessionTests, TransactionSnapshotsKeepObjectValuesAlive)
{
	InitializeDObjectSystem();
	auto* Owner = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "ObjectValueOwner");
	auto* Property = DReflectedTransactionTestObject::FindProperty("ObjectValue");
	Durin::DObject* Before = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectValueBefore"));
	Durin::DObject* After = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectValueAfter"));
	Owner->ObjectValue = After;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Owner, 0, Proposed));
	Owner->ObjectValue = Before;
	const auto Target = Durin::Editor::FPropertyEditTarget::ForMember(Owner, Property);
	FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Object Value", nullptr, Transactions.Get()));
	ASSERT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);

	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Owner));
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Before));
	ASSERT_TRUE(Durin::GDObjectArray.Contains(After));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Owner->ObjectValue.Get(), Before);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Owner->ObjectValue.Get(), After);

	Transactions.Get()->Reset();
	Owner->ObjectValue = nullptr;
	Proposed = {};
	Durin::CollectGarbage();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Owner));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Before));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(After));
}
