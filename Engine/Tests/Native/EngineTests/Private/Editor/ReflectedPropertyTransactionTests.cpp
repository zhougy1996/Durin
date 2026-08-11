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

TEST(FReflectedPropertyEditSessionTests, ContinuousCommitCreatesOneUndoRedoTransaction)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	DEditObserver Object;
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", nullptr, &Transactions));

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 11)), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 19)), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_TRUE(Transactions.CanUndo());
	EXPECT_FALSE(Transactions.CanRedo());
	EXPECT_EQ(Transactions.GetUndoDescription(), "Edit Value");
	ASSERT_EQ(Transactions.ConsumeEvents().size(), 1u);

	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Value, 7);
	EXPECT_FALSE(Transactions.CanUndo());
	EXPECT_TRUE(Transactions.CanRedo());
	ASSERT_EQ(Object.Changes.size(), 4u);
	EXPECT_EQ(Object.Changes[2].Phase, Durin::EPropertyChangePhase::Committed);
	EXPECT_EQ(Object.Changes[2].Origin, Durin::EPropertyChangeOrigin::Edit);
	EXPECT_EQ(Object.Changes[3].Phase, Durin::EPropertyChangePhase::Committed);
	EXPECT_EQ(Object.Changes[3].Origin, Durin::EPropertyChangeOrigin::Undo);

	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Container.Value, 19);
	EXPECT_TRUE(Transactions.CanUndo());
	EXPECT_FALSE(Transactions.CanRedo());
	ASSERT_EQ(Object.Changes.size(), 5u);
	EXPECT_EQ(Object.Changes[4].Origin, Durin::EPropertyChangeOrigin::Redo);
	const std::vector<Durin::Editor::FTransactionEvent> Events = Transactions.ConsumeEvents();
	ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events[0].Type, Durin::Editor::ETransactionEventType::Undone);
	EXPECT_EQ(Events[1].Type, Durin::Editor::ETransactionEventType::Redone);
}

TEST(FReflectedPropertyEditSessionTests, SynchronizesPackageDirtyStateAtSavedRevision)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	Durin::DPackage* Package = MakeReflectedRevisionTestPackage();
	Durin::DObject* Object = Durin::NewObject<Durin::DObject>(Package, Durin::FName("PropertyEditTarget"));
	Durin::Editor::FPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.SnapshotProperty = Property.get();
	Target.SnapshotContainer = &Container;
	Target.Path.push_back({Property.get()});
	Durin::Editor::FTransactionManager Transactions;
	Transactions.EstablishSavedState(*Package);

	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Saved Value", nullptr, &Transactions));
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 19)), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Value, 7);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Container.Value, 19);
	EXPECT_TRUE(Package->IsDirty());
	Transactions.MarkSaved(*Package);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_FALSE(Package->IsDirty());
}

TEST(FReflectedPropertyEditSessionTests, NoOpAndCancelledEditsDoNotCreateTransactions)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{3};
	DEditObserver Object;
	Durin::Editor::FTransactionManager Transactions;

	Durin::Editor::FPropertyEditSession NoOpSession;
	ASSERT_TRUE(NoOpSession.Begin(MakeTarget(Object, Property.get(), Container), "No-op", nullptr, &Transactions));
	EXPECT_EQ(NoOpSession.Commit(), Durin::Editor::EPropertyEditResult::NoChange);

	Durin::Editor::FPropertyEditSession CancelledSession;
	ASSERT_TRUE(CancelledSession.Begin(MakeTarget(Object, Property.get(), Container), "Cancelled", nullptr, &Transactions));
	ASSERT_EQ(CancelledSession.Apply(CaptureValue(Property.get(), Container, 9)), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(CancelledSession.Cancel(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 3);
	EXPECT_FALSE(Transactions.CanUndo());
	EXPECT_TRUE(Transactions.ConsumeEvents().empty());
}

TEST(FReflectedPropertyEditSessionTests, TransactionHistoryKeepsTargetAlive)
{
	InitializeDObjectSystem();
	auto Property = MakeValueProperty();
	FValueContainer Container{4};
	Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("TransactionalPropertyTarget"));
	Durin::Editor::FPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.SnapshotProperty = Property.get();
	Target.SnapshotContainer = &Container;
	Target.Path.push_back({Property.get()});
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Rooted Value", nullptr, &Transactions));
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 12)), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);

	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Object));
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Value, 4);
	Transactions.Clear();
	Durin::CollectGarbage();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Object));
}

TEST(FReflectedPropertyEditSessionTests, TransactionSnapshotsKeepObjectValuesAlive)
{
	InitializeDObjectSystem();
	Durin::FObjectProperty Property(
		Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
		Durin::EPropertyFlags::Edit, 1, static_cast<Durin::uint16>(offsetof(FObjectValueContainer, Value)),
		static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
		Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(), true,
		[](const void* Value) -> Durin::DObject* {
			return static_cast<const Durin::TObjectPtr<Durin::DObject>*>(Value)->Get();
		},
		[](void* Value, Durin::DObject* Object) {
			*static_cast<Durin::TObjectPtr<Durin::DObject>*>(Value) = Object;
		}
	);
	SetTestValueLifecycle<Durin::TObjectPtr<Durin::DObject>>(Property);
	Durin::DObject* Owner = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectValueOwner"));
	Durin::DObject* Before = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectValueBefore"));
	Durin::DObject* After = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectValueAfter"));
	FObjectValueContainer Container{Before};
	FObjectValueContainer ProposedContainer{After};
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(&Property, &ProposedContainer, 0, Proposed));
	Durin::Editor::FPropertyEditTarget Target;
	Target.Object = Owner;
	Target.MemberProperty = &Property;
	Target.LeafProperty = &Property;
	Target.SnapshotProperty = &Property;
	Target.SnapshotContainer = &Container;
	Target.Path.push_back({&Property});
	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Object Value", nullptr, &Transactions));
	ASSERT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);

	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Owner));
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Before));
	ASSERT_TRUE(Durin::GDObjectArray.Contains(After));
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Value.Get(), Before);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Container.Value.Get(), After);

	Transactions.Clear();
	Container.Value = nullptr;
	ProposedContainer.Value = nullptr;
	Proposed = {};
	Durin::CollectGarbage();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Owner));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Before));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(After));
}
