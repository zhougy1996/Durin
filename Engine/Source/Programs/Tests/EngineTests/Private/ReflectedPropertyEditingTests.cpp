#include "Editor/ReflectedPropertyEditing.h"
#include "Editor/EditorTransaction.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectArray.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "EngineTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	struct FValueContainer
	{
		Durin::int32 Value = 0;
	};

	struct FObjectValueContainer
	{
		Durin::TObjectPtr<Durin::DObject> Value;
	};

	struct FCapturedChange
	{
		Durin::EPropertyChangePhase Phase = Durin::EPropertyChangePhase::Committed;
		Durin::EPropertyChangeKind Kind = Durin::EPropertyChangeKind::ValueSet;
		Durin::EPropertyChangeOrigin Origin = Durin::EPropertyChangeOrigin::Edit;
		std::vector<Durin::uint8> MapKeyData;
	};

	class DEditObserver final : public Durin::DObject
	{
	public:
		auto PostEditChangeProperty(const Durin::FPropertyChangedEvent& Event) -> void override
		{
			FCapturedChange& Change = Changes.emplace_back();
			Change.Phase = Event.Phase;
			Change.Kind = Event.Kind;
			Change.Origin = Event.Origin;
			if (!Event.Path.empty()) Change.MapKeyData.assign(Event.Path.front().MapKeyData.begin(), Event.Path.front().MapKeyData.end());
		}

		std::vector<FCapturedChange> Changes;
	};

	class FRejectingMutationAdapter final : public Durin::IReflectedPropertyMutationAdapter
	{
	public:
		auto Capture(const Durin::FReflectedPropertyEditTarget& Target, Durin::FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool override
		{
			return Durin::CapturePropertyValue(Target.LeafProperty, Target.LeafContainer, Target.LeafArrayIndex, OutSnapshot, OutError);
		}
		auto Apply(const Durin::FReflectedPropertyEditTarget&, const Durin::FPropertyValueSnapshot&, std::string* OutError) const -> bool override
		{
			if (OutError) *OutError = "Rejected for testing.";
			return false;
		}
		auto Restore(const Durin::FReflectedPropertyEditTarget&, const Durin::FPropertyValueSnapshot&, std::string*) const -> bool override
		{
			return false;
		}
	};

	class FUndoRejectingMutationAdapter final : public Durin::IReflectedPropertyMutationAdapter
	{
	public:
		auto Capture(const Durin::FReflectedPropertyEditTarget& Target, Durin::FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool override
		{
			return Durin::GetGenericReflectedPropertyMutationAdapter().Capture(Target, OutSnapshot, OutError);
		}
		auto Apply(const Durin::FReflectedPropertyEditTarget& Target, const Durin::FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool override
		{
			return Durin::GetGenericReflectedPropertyMutationAdapter().Apply(Target, Snapshot, OutError);
		}
		auto Restore(const Durin::FReflectedPropertyEditTarget&, const Durin::FPropertyValueSnapshot&, std::string* OutError) const -> bool override
		{
			if (OutError) *OutError = "Undo rejected for testing.";
			return false;
		}
	};

	auto MakeValueProperty() -> std::unique_ptr<Durin::FNumericProperty>
	{
		return std::make_unique<Durin::FNumericProperty>(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, static_cast<Durin::uint16>(offsetof(FValueContainer, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::int32)), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
	}

	auto CaptureValue(const Durin::FProperty* Property, FValueContainer& Container, Durin::int32 Value) -> Durin::FPropertyValueSnapshot
	{
		FValueContainer Proposed{Value};
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Property, &Proposed, 0, Snapshot));
		return Snapshot;
	}

	auto MakeTarget(DEditObserver& Object, const Durin::FProperty* Property, FValueContainer& Container) -> Durin::FReflectedPropertyEditTarget
	{
		Durin::FReflectedPropertyEditTarget Target;
		Target.Object = &Object;
		Target.MemberProperty = Property;
		Target.LeafProperty = Property;
		Target.LeafContainer = &Container;
		Target.Path.push_back({Property});
		return Target;
	}
}

TEST(FReflectedPropertyEditSessionTests, AppliesInteractiveValuesAndCommitsOnce)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	DEditObserver Object;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value"));
	const Durin::FPropertyValueSnapshot Proposed = CaptureValue(Property.get(), Container, 19);

	EXPECT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 19);
	EXPECT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::NoChange);
	ASSERT_EQ(Object.Changes.size(), 1u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Committed);
	EXPECT_FALSE(Session.IsActive());
}

TEST(FReflectedPropertyEditSessionTests, CancelRestoresOriginalValueAndOwnedPathData)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{3};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget Target = MakeTarget(Object, Property.get(), Container);
	Target.Path[0].Selector = Durin::EPropertyPathSelector::MapKey;
	Target.Path[0].MapKeyData = {1, 2, 3};
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Nested Value"));
	Target.Path[0].MapKeyData[0] = 9;

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 11)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 3);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[0].MapKeyData, (std::vector<Durin::uint8>{1, 2, 3}));
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, RejectsMutationWithoutChangingOrNotifying)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	DEditObserver Object;
	FRejectingMutationAdapter Adapter;
	Durin::FReflectedPropertyEditSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", &Adapter, &Error)) << Error;

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8), &Error), Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_EQ(Container.Value, 5);
	EXPECT_TRUE(Object.Changes.empty());
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::NoChange);
}

TEST(FReflectedPropertyEditSessionTests, NoOpCommitAndSessionDestructionDoNotAbandonPreviewState)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{13};
	DEditObserver Object;
	{
		Durin::FReflectedPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "No-op Edit"));
		EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::NoChange);
		EXPECT_TRUE(Object.Changes.empty());
	}

	{
		Durin::FReflectedPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Abandoned Preview"));
		EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 27)), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Container.Value, 27);
	}
	EXPECT_EQ(Container.Value, 13);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, KeepsRegisteredTargetAliveForTheSession)
{
	InitializeDObjectSystem();
	auto Property = MakeValueProperty();
	FValueContainer Container{4};
	Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PropertyEditTarget"));
	Durin::FReflectedPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.LeafContainer = &Container;
	Target.Path.push_back({Property.get()});

	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Rooted Edit"));
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Object));
	ASSERT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::NoChange);
	Durin::CollectGarbage();
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Object));
}

TEST(FReflectedPropertyEditSessionTests, ContinuousCommitCreatesOneUndoRedoTransaction)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	DEditObserver Object;
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", nullptr, nullptr, &Transactions));

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 11)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 19)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
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
	const std::vector<Durin::FEditorTransactionEvent> Events = Transactions.ConsumeEvents();
	ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events[0].Type, Durin::EEditorTransactionEventType::Undone);
	EXPECT_EQ(Events[1].Type, Durin::EEditorTransactionEventType::Redone);
}

TEST(FReflectedPropertyEditSessionTests, NoOpAndCancelledEditsDoNotCreateTransactions)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{3};
	DEditObserver Object;
	Durin::FEditorTransactionManager Transactions;

	Durin::FReflectedPropertyEditSession NoOpSession;
	ASSERT_TRUE(NoOpSession.Begin(MakeTarget(Object, Property.get(), Container), "No-op", nullptr, nullptr, &Transactions));
	EXPECT_EQ(NoOpSession.Commit(), Durin::EReflectedPropertyEditResult::NoChange);

	Durin::FReflectedPropertyEditSession CancelledSession;
	ASSERT_TRUE(CancelledSession.Begin(MakeTarget(Object, Property.get(), Container), "Cancelled", nullptr, nullptr, &Transactions));
	ASSERT_EQ(CancelledSession.Apply(CaptureValue(Property.get(), Container, 9)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(CancelledSession.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 3);
	EXPECT_FALSE(Transactions.CanUndo());
	EXPECT_TRUE(Transactions.ConsumeEvents().empty());
}

TEST(FReflectedPropertyEditSessionTests, FailedUndoKeepsHistoryAndReportsAdapterError)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	DEditObserver Object;
	FUndoRejectingMutationAdapter Adapter;
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", &Adapter, nullptr, &Transactions));
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8)), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	Transactions.ConsumeEvents();

	EXPECT_FALSE(Transactions.Undo());
	EXPECT_EQ(Container.Value, 8);
	EXPECT_TRUE(Transactions.CanUndo());
	EXPECT_FALSE(Transactions.CanRedo());
	const std::vector<Durin::FEditorTransactionEvent> Events = Transactions.ConsumeEvents();
	ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events[0].Type, Durin::EEditorTransactionEventType::Failed);
	EXPECT_EQ(Events[0].Operation, Durin::EEditorTransactionOperation::Undo);
	EXPECT_EQ(Events[0].Details, "Undo rejected for testing.");
}

TEST(FReflectedPropertyEditSessionTests, TransactionHistoryKeepsRegisteredTargetAlive)
{
	InitializeDObjectSystem();
	auto Property = MakeValueProperty();
	FValueContainer Container{4};
	Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("TransactionalPropertyTarget"));
	Durin::FReflectedPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.LeafContainer = &Container;
	Target.Path.push_back({Property.get()});
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Rooted Value", nullptr, nullptr, &Transactions));
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 12)), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);

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
		Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(), true
	);
	Durin::DObject* Owner = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectValueOwner"));
	Durin::DObject* Before = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectValueBefore"));
	Durin::DObject* After = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("ObjectValueAfter"));
	FObjectValueContainer Container{Before};
	FObjectValueContainer ProposedContainer{After};
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(&Property, &ProposedContainer, 0, Proposed));
	Durin::FReflectedPropertyEditTarget Target;
	Target.Object = Owner;
	Target.MemberProperty = &Property;
	Target.LeafProperty = &Property;
	Target.LeafContainer = &Container;
	Target.Path.push_back({&Property});
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Object Value", nullptr, nullptr, &Transactions));
	ASSERT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);

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
