#include "Editor/ReflectedPropertyEditing.h"
#include "Editor/EditorTransaction.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectArray.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "Components/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstance.h"
#include "EngineTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	struct FValueContainer
	{
		Durin::int32 Value = 0;
	};

	struct FGuidValueContainer
	{
		Durin::FGuid Value;
	};

	struct FObjectValueContainer
	{
		Durin::TObjectPtr<Durin::DObject> Value;
	};

	struct FArrayValueContainer
	{
		std::vector<Durin::int32> Values;
	};

	struct FMapValueContainer
	{
		std::unordered_map<std::string, Durin::int32> Values;
	};

	template<typename T>
	auto InitializeTestValue(void* Memory) -> void { std::construct_at(static_cast<T*>(Memory)); }
	template<typename T>
	auto DestroyTestValue(void* Memory) -> void { std::destroy_at(static_cast<T*>(Memory)); }
	template<typename T>
	auto SetTestValueLifecycle(Durin::FProperty& Property) -> void
	{
		Property.SetValueLifecycle(sizeof(T), alignof(T), &InitializeTestValue<T>, &DestroyTestValue<T>);
	}

	struct FCapturedChange
	{
		Durin::EPropertyChangePhase Phase = Durin::EPropertyChangePhase::Committed;
		Durin::EPropertyChangeKind Kind = Durin::EPropertyChangeKind::ValueSet;
		Durin::EPropertyChangeOrigin Origin = Durin::EPropertyChangeOrigin::Edit;
		const Durin::FProperty* MemberProperty = nullptr;
		const Durin::FProperty* LeafProperty = nullptr;
		std::vector<Durin::EPropertyPathSelector> Selectors;
		std::vector<Durin::uint64> Indices;
		std::vector<Durin::uint8> MapKeyData;
	};

	class DEditObserver final : public Durin::DObject
	{
	public:
		auto PreEditChangeProperty(Durin::FPropertyEditProposal& Proposal, std::string& OutError) -> bool override
		{
			++PreChangeCount;
			LastProposalPhase = Proposal.Phase;
			LastProposalOrigin = Proposal.Origin;
			LastProposalKind = Proposal.Kind;
			bLastProposalHadLeaf = Proposal.DraftLeafContainer != nullptr;
			return PreChange ? PreChange(Proposal, OutError) : true;
		}

		auto PostEditChangeProperty(const Durin::FPropertyChangedEvent& Event) -> void override
		{
			FCapturedChange& Change = Changes.emplace_back();
			Change.Phase = Event.Phase;
			Change.Kind = Event.Kind;
			Change.Origin = Event.Origin;
			Change.MemberProperty = Event.MemberProperty;
			Change.LeafProperty = Event.LeafProperty;
			for (const Durin::FPropertyPathSegment& Segment : Event.Path)
			{
				Change.Selectors.push_back(Segment.Selector);
				Change.Indices.push_back(Segment.Index);
				if (!Segment.MapKeyData.empty()) Change.MapKeyData.assign(Segment.MapKeyData.begin(), Segment.MapKeyData.end());
			}
		}

		std::vector<FCapturedChange> Changes;
		std::function<bool(Durin::FPropertyEditProposal&, std::string&)> PreChange;
		Durin::uint32 PreChangeCount = 0;
		Durin::EPropertyChangePhase LastProposalPhase = Durin::EPropertyChangePhase::Interactive;
		Durin::EPropertyChangeOrigin LastProposalOrigin = Durin::EPropertyChangeOrigin::Edit;
		Durin::EPropertyChangeKind LastProposalKind = Durin::EPropertyChangeKind::ValueSet;
		bool bLastProposalHadLeaf = false;
	};

	auto MakeValueProperty() -> std::unique_ptr<Durin::FNumericProperty>
	{
		auto Property = std::make_unique<Durin::FNumericProperty>(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, static_cast<Durin::uint16>(offsetof(FValueContainer, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::int32)), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		SetTestValueLifecycle<Durin::int32>(*Property);
		return Property;
	}

	auto MakeGuidProperty() -> std::unique_ptr<Durin::FGuidProperty>
	{
		auto Property = std::make_unique<Durin::FGuidProperty>(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, static_cast<Durin::uint16>(offsetof(FGuidValueContainer, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::FGuid)), Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		SetTestValueLifecycle<Durin::FGuid>(*Property);
		return Property;
	}

	template<typename T>
	auto VectorNum(const void* Container) -> Durin::uint64 { return static_cast<Durin::uint64>(static_cast<const std::vector<T>*>(Container)->size()); }
	template<typename T>
	auto VectorElement(const void* Container, Durin::uint64 Index) -> const void* { return &(*static_cast<const std::vector<T>*>(Container))[static_cast<size_t>(Index)]; }
	template<typename T>
	auto MutableVectorElement(void* Container, Durin::uint64 Index) -> void* { return &(*static_cast<std::vector<T>*>(Container))[static_cast<size_t>(Index)]; }
	template<typename T>
	auto ResizeVector(void* Container, Durin::uint64 Num) -> void { static_cast<std::vector<T>*>(Container)->resize(static_cast<size_t>(Num)); }

	const Durin::DurinCodeGen::FArrayPropertyHelper GIntArrayHelper = {
		&VectorNum<Durin::int32>, &VectorElement<Durin::int32>, &MutableVectorElement<Durin::int32>, &ResizeVector<Durin::int32>
	};

	using FStringIntMap = std::unordered_map<std::string, Durin::int32>;
	auto MapNum(const void* Container) -> Durin::uint64 { return static_cast<Durin::uint64>(static_cast<const FStringIntMap*>(Container)->size()); }
	auto MapKey(const void* Container, Durin::uint64 Index) -> const void* { auto It = static_cast<const FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->first; }
	auto MapValue(const void* Container, Durin::uint64 Index) -> const void* { auto It = static_cast<const FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->second; }
	auto MutableMapValue(void* Container, Durin::uint64 Index) -> void* { auto It = static_cast<FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->second; }
	auto ClearMap(void* Container) -> void { static_cast<FStringIntMap*>(Container)->clear(); }
	auto CreateMapKey() -> void* { return new std::string(); }
	auto CopyMapKey(const void* Key) -> void* { return new std::string(*static_cast<const std::string*>(Key)); }
	auto DestroyMapKey(void* Key) -> void { delete static_cast<std::string*>(Key); }
	auto CreateMapValue() -> void* { return new Durin::int32(); }
	auto DestroyMapValue(void* Value) -> void { delete static_cast<Durin::int32*>(Value); }
	auto InsertMap(void* Container, const void* Key, const void* Value) -> void { static_cast<FStringIntMap*>(Container)->insert_or_assign(*static_cast<const std::string*>(Key), *static_cast<const Durin::int32*>(Value)); }
	auto ContainsMap(const void* Container, const void* Key) -> bool { return static_cast<const FStringIntMap*>(Container)->contains(*static_cast<const std::string*>(Key)); }
	auto RenameMapKey(void* Container, const void* OldKey, const void* NewKey) -> bool
	{
		auto* Map = static_cast<FStringIntMap*>(Container);
		const std::string Old = *static_cast<const std::string*>(OldKey);
		const std::string New = *static_cast<const std::string*>(NewKey);
		if (Old == New || Map->contains(New)) return false;
		auto Node = Map->extract(Old);
		if (Node.empty()) return false;
		Node.key() = New;
		Map->insert(std::move(Node));
		return true;
	}
	auto RemoveMap(void* Container, const void* Key) -> bool { return static_cast<FStringIntMap*>(Container)->erase(*static_cast<const std::string*>(Key)) != 0; }

	const Durin::DurinCodeGen::FMapPropertyHelper GStringIntMapHelper = {
		&MapNum, &MapKey, &MapValue, &MutableMapValue, &ClearMap,
		&CreateMapKey, &CopyMapKey, &DestroyMapKey, &CreateMapValue, &DestroyMapValue,
		&InsertMap, &ContainsMap, &RenameMapKey, &RemoveMap
	};

	auto MakeArrayProperty(Durin::FNumericProperty& Inner) -> std::unique_ptr<Durin::FArrayProperty>
	{
		auto Property = std::make_unique<Durin::FArrayProperty>(
			Durin::FFieldVariant(), Durin::FName("Values"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
			1, static_cast<Durin::uint16>(offsetof(FArrayValueContainer, Values)), static_cast<Durin::uint16>(sizeof(std::vector<Durin::int32>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, &GIntArrayHelper
		);
		Property->SetInner(&Inner);
		SetTestValueLifecycle<std::vector<Durin::int32>>(*Property);
		return Property;
	}

	auto MakeMapProperty(Durin::FStringProperty& Key, Durin::FNumericProperty& Value) -> std::unique_ptr<Durin::FMapProperty>
	{
		auto Property = std::make_unique<Durin::FMapProperty>(
			Durin::FFieldVariant(), Durin::FName("Values"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
			1, static_cast<Durin::uint16>(offsetof(FMapValueContainer, Values)), static_cast<Durin::uint16>(sizeof(FStringIntMap)),
			Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, &GStringIntMapHelper
		);
		Property->SetKeyProp(&Key);
		Property->SetValueProp(&Value);
		SetTestValueLifecycle<FStringIntMap>(*Property);
		return Property;
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
		Target.SnapshotProperty = Property;
		Target.SnapshotContainer = &Container;
		Target.Path.push_back({Property});
		return Target;
	}
}

TEST(FReflectedPropertyEditSessionTests, CommitsAndUndoRedoesGuidValues)
{
	auto Property = MakeGuidProperty();
	const Durin::FGuid Original(1, 2, 3, 4);
	const Durin::FGuid Proposed(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
	FGuidValueContainer Container{Original};
	FGuidValueContainer ProposedContainer{Proposed};
	DEditObserver Object;
	Durin::FPropertyValueSnapshot ProposedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property.get(), &ProposedContainer, 0, ProposedSnapshot));

	Durin::FReflectedPropertyEditTarget Target;
	Target.Object = &Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.SnapshotProperty = Property.get();
	Target.SnapshotContainer = &Container;
	Target.Path.push_back({Property.get()});
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Guid", nullptr, &Transactions));
	EXPECT_EQ(Session.Apply(ProposedSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, Proposed);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Value, Original);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Container.Value, Proposed);
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

TEST(FReflectedPropertyEditSessionTests, GenericHookRejectsAndNormalizesDetachedProposalsAtomically)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	DEditObserver Object;
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Validated Edit", nullptr, &Transactions));

	Object.PreChange = [](Durin::FPropertyEditProposal&, std::string& Error) {
		Error = "Rejected detached proposal.";
		return false;
	};
	std::string Error;
	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 19), &Error), Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_EQ(Error, "Rejected detached proposal.");
	EXPECT_EQ(Container.Value, 7);
	EXPECT_TRUE(Object.Changes.empty());
	EXPECT_FALSE(Transactions.CanUndo());

	Object.PreChange = [Property = Property.get()](Durin::FPropertyEditProposal& Proposal, std::string&) {
		auto* Value = Property->ContainerPtrToValuePtr<Durin::int32>(Proposal.DraftLeafContainer, Proposal.DraftLeafArrayIndex);
		*Value = std::clamp(*Value, 0, 10);
		return true;
	};
	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 19)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 10);
	FValueContainer Normalized{10};
	Durin::FPropertyValueSnapshot NormalizedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property.get(), &Normalized, 0, NormalizedSnapshot));
	EXPECT_EQ(Session.GetCurrentValue(), NormalizedSnapshot);
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Value, 7);
	EXPECT_EQ(Object.LastProposalPhase, Durin::EPropertyChangePhase::Committed);
	EXPECT_EQ(Object.LastProposalOrigin, Durin::EPropertyChangeOrigin::Undo);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Container.Value, 10);
}

TEST(FReflectedPropertyEditSessionTests, GenericHookRejectsNestedEditOfSameTarget)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{2};
	DEditObserver Object;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Reentrant Edit"));
	const Durin::FPropertyValueSnapshot NestedProposal = CaptureValue(Property.get(), Container, 6);
	Durin::EReflectedPropertyEditResult NestedResult = Durin::EReflectedPropertyEditResult::Changed;
	std::string NestedError;
	Object.PreChange = [&](Durin::FPropertyEditProposal&, std::string&) {
		NestedResult = Session.Apply(NestedProposal, &NestedError);
		return true;
	};

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 9)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(NestedResult, Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_EQ(NestedError, "A reflected property hook cannot start a nested edit of the same target.");
	EXPECT_EQ(Container.Value, 9);
	EXPECT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
}

TEST(FReflectedPropertyEditSessionTests, GeneratesDefaultDescriptionOnlyForValidTargets)
{
	Durin::FReflectedPropertyEditSession Session;
	std::string Error;
	EXPECT_FALSE(Session.Begin({}, {}, &Error));
	EXPECT_EQ(Error, "The edit target has no owning object.");
	EXPECT_FALSE(Session.IsActive());

	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget Incomplete = MakeTarget(Object, Property.get(), Container);
	Incomplete.SnapshotProperty = nullptr;
	Incomplete.SnapshotContainer = nullptr;
	EXPECT_FALSE(Session.Begin(Incomplete, {}, &Error));
	EXPECT_EQ(Error, "The edit target is incomplete.");
	EXPECT_FALSE(Session.IsActive());

	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), {}, &Error)) << Error;
	EXPECT_EQ(Session.GetDescription(), "Edit Value");
	EXPECT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::NoChange);
	ASSERT_EQ(Object.Changes.size(), 1u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, CancelRestoresOriginalValueAndOwnedPathData)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{3};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget Target = MakeTarget(Object, Property.get(), Container);
	Target.Path[0].Index = 3;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Nested Value"));
	Target.Path[0].Index = 9;

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 11)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 3);
	ASSERT_EQ(Object.Changes.size(), 2u);
	ASSERT_EQ(Object.Changes[0].Indices.size(), 1u);
	EXPECT_EQ(Object.Changes[0].Indices[0], 3u);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, RejectsMutationWithoutChangingOrNotifying)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	DEditObserver Object;
	Object.PreChange = [](Durin::FPropertyEditProposal&, std::string& OutError) {
		OutError = "Rejected for testing.";
		return false;
	};
	Durin::FReflectedPropertyEditSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", &Error)) << Error;

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8), &Error), Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_EQ(Container.Value, 5);
	EXPECT_TRUE(Object.Changes.empty());
	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::NoChange);
	ASSERT_EQ(Object.Changes.size(), 1u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Committed);
}

TEST(FReflectedPropertyEditSessionTests, FailedCancelKeepsSessionRecoverableForRetry)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	DEditObserver Object;
	bool bAllowRestore = false;
	Object.PreChange = [&](Durin::FPropertyEditProposal& Proposal, std::string& OutError) {
		if (Proposal.Phase == Durin::EPropertyChangePhase::Cancelled && !bAllowRestore)
		{
			OutError = "Restore rejected for testing.";
			return false;
		}
		return true;
	};
	Durin::FReflectedPropertyEditSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", &Error)) << Error;
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8)), Durin::EReflectedPropertyEditResult::Changed);

	EXPECT_EQ(Session.Cancel(&Error), Durin::EReflectedPropertyEditResult::Failed);
	EXPECT_EQ(Error, "Restore rejected for testing.");
	EXPECT_TRUE(Session.IsActive());
	EXPECT_EQ(Container.Value, 8);
	bAllowRestore = true;
	EXPECT_EQ(Session.Cancel(&Error), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_FALSE(Session.IsActive());
	EXPECT_EQ(Container.Value, 5);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, EmitsTerminalEventAfterReturningToOriginalValue)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	DEditObserver Object;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value"));
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8)), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 5)), Durin::EReflectedPropertyEditResult::Changed);

	EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::NoChange);
	ASSERT_EQ(Object.Changes.size(), 3u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Object.Changes[2].Phase, Durin::EPropertyChangePhase::Committed);
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
		ASSERT_EQ(Object.Changes.size(), 1u);
		EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Committed);
	}

	{
		Durin::FReflectedPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Abandoned Preview"));
		EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 27)), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Container.Value, 27);
	}
	EXPECT_EQ(Container.Value, 13);
	ASSERT_EQ(Object.Changes.size(), 3u);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Object.Changes[2].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, KeepsTargetAliveForTheSession)
{
	InitializeDObjectSystem();
	auto Property = MakeValueProperty();
	FValueContainer Container{4};
	Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PropertyEditTarget"));
	Durin::FReflectedPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.SnapshotProperty = Property.get();
	Target.SnapshotContainer = &Container;
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
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", nullptr, &Transactions));

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
	ASSERT_TRUE(NoOpSession.Begin(MakeTarget(Object, Property.get(), Container), "No-op", nullptr, &Transactions));
	EXPECT_EQ(NoOpSession.Commit(), Durin::EReflectedPropertyEditResult::NoChange);

	Durin::FReflectedPropertyEditSession CancelledSession;
	ASSERT_TRUE(CancelledSession.Begin(MakeTarget(Object, Property.get(), Container), "Cancelled", nullptr, &Transactions));
	ASSERT_EQ(CancelledSession.Apply(CaptureValue(Property.get(), Container, 9)), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(CancelledSession.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
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
	Durin::FReflectedPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.SnapshotProperty = Property.get();
	Target.SnapshotContainer = &Container;
	Target.Path.push_back({Property.get()});
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Rooted Value", nullptr, &Transactions));
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
	SetTestValueLifecycle<Durin::TObjectPtr<Durin::DObject>>(Property);
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
	Target.SnapshotProperty = &Property;
	Target.SnapshotContainer = &Container;
	Target.Path.push_back({&Property});
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Object Value", nullptr, &Transactions));
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

TEST(FReflectedPropertyEditSessionTests, GenericHookPipelineAppliesNestedStructField)
{
	InitializeDObjectSystem();
	auto* Camera = Durin::NewObject<Durin::DCameraComponent>(nullptr, "GenericNestedCamera");
	auto* ProposedCamera = Durin::NewObject<Durin::DCameraComponent>(nullptr, "GenericNestedProposal");
	auto* Projection = static_cast<Durin::FStructProperty*>(Camera->GetClass()->FindPropertyByName("ProjectionSettings"));
	ASSERT_NE(Projection, nullptr);
	auto* NearClip = Projection->GetStruct()->FindPropertyByName(Durin::FName("NearClip"));
	ASSERT_NE(NearClip, nullptr);

	Durin::FCameraProjectionSettings ProposedSettings = ProposedCamera->GetProjectionSettings();
	ProposedSettings.NearClip = 4.0f;
	ProposedCamera->SetProjectionSettings(ProposedSettings);
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Projection, ProposedCamera, 0, Proposed));
	const Durin::FReflectedPropertyEditTarget Target = Durin::FReflectedPropertyEditTarget::ForMember(Camera, Projection)
		.ForStructMember(NearClip);
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Generic Nested Edit"));
	EXPECT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 4.0f);
	EXPECT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 0.1f);

	Durin::MarkAsGarbage(Camera);
	Durin::MarkAsGarbage(ProposedCamera);
	Durin::CollectGarbage();
}

TEST(FReflectedPropertyEditSessionTests, RelativeTransformHookNormalizesAndRefreshesHierarchyAcrossCancel)
{
	InitializeDObjectSystem();
	auto* Parent = Durin::NewObject<Durin::DSceneComponent>(nullptr, "HookTransformParent");
	auto* Child = Durin::NewObject<Durin::DSceneComponent>(nullptr, "HookTransformChild");
	auto* ProposalObject = Durin::NewObject<Durin::DSceneComponent>(nullptr, "HookTransformProposal");
	Durin::FTransform ParentTransform;
	ParentTransform.Translation = {10.0, 0.0, 0.0};
	Parent->SetRelativeTransform(ParentTransform);
	ASSERT_TRUE(Child->SetupAttachment(Parent));

	auto* Property = Child->GetClass()->FindPropertyByName(Durin::FName("RelativeTransform"));
	ASSERT_NE(Property, nullptr);
	auto* ProposedTransform = Property->ContainerPtrToValuePtr<Durin::FTransform>(ProposalObject);
	ProposedTransform->Translation = {3.0, 0.0, 0.0};
	ProposedTransform->Rotation *= 2.0;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, ProposalObject, 0, Proposed));

	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::FReflectedPropertyEditTarget::ForMember(Child, Property), "Edit Transform"));
	ASSERT_EQ(Session.Apply(Proposed), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_NEAR(glm::length(Child->GetRelativeRotation()), 1.0, 1.e-8);
	EXPECT_DOUBLE_EQ(Child->GetWorldLocation().x, 13.0);
	ASSERT_EQ(Session.Cancel(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_DOUBLE_EQ(Child->GetWorldLocation().x, 10.0);

	Child->DetachFromComponent(Durin::EDetachmentTransformRule::KeepWorld);
	Durin::MarkAsGarbage(ProposalObject);
	Durin::MarkAsGarbage(Child);
	Durin::MarkAsGarbage(Parent);
	Durin::CollectGarbage();
}

TEST(FReflectedPropertyEditSessionTests, ArrayElementUsesStableContainerSnapshotAndExactPath)
{
	auto Inner = MakeValueProperty();
	auto Array = MakeArrayProperty(*Inner);
	FArrayValueContainer Container{{3, 7, 11}};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget ArrayTarget = Durin::FReflectedPropertyEditTarget::ForMember(&Object, Array.get());
	ArrayTarget.SnapshotContainer = &Container;
	Durin::FReflectedPropertyEditTarget ElementTarget = ArrayTarget.ForArrayElement(Inner.get(), 1);
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(ElementTarget, "Edit Array Element", nullptr, &Transactions));

	FArrayValueContainer FirstProposal{{3, 19, 11}};
	Durin::FPropertyValueSnapshot FirstSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Array.get(), &FirstProposal, 0, FirstSnapshot));
	ASSERT_EQ(Session.Apply(FirstSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	// Whole-container restore is allowed to move storage, but path identity must
	// keep the same continuous widget in one edit session.
	ElementTarget = ArrayTarget.ForArrayElement(Inner.get(), 1);
	EXPECT_TRUE(Session.MatchesTarget(ElementTarget));
	FArrayValueContainer SecondProposal{{3, 23, 11}};
	Durin::FPropertyValueSnapshot SecondSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Array.get(), &SecondProposal, 0, SecondSnapshot));
	ASSERT_EQ(Session.Apply(SecondSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Container.Values, (std::vector<Durin::int32>{3, 23, 11}));

	ASSERT_EQ(Object.Changes.size(), 3u);
	const FCapturedChange& Commit = Object.Changes.back();
	EXPECT_EQ(Commit.MemberProperty, Array.get());
	EXPECT_EQ(Commit.LeafProperty, Inner.get());
	ASSERT_EQ(Commit.Selectors.size(), 2u);
	EXPECT_EQ(Commit.Selectors[0], Durin::EPropertyPathSelector::ArrayIndex);
	EXPECT_EQ(Commit.Indices[0], 1u);
	EXPECT_EQ(Commit.Selectors[1], Durin::EPropertyPathSelector::None);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{3, 7, 11}));
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{3, 23, 11}));
}

TEST(FReflectedPropertyEditSessionTests, ArrayStructuralKindsRestoreRemovedAndResizedElements)
{
	auto Inner = MakeValueProperty();
	auto Array = MakeArrayProperty(*Inner);
	FArrayValueContainer Container{{4, 8, 15}};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget Target = Durin::FReflectedPropertyEditTarget::ForMember(&Object, Array.get());
	Target.SnapshotContainer = &Container;
	Durin::FEditorTransactionManager Transactions;

	auto CommitValues = [&](std::vector<Durin::int32> Values, Durin::EPropertyChangeKind Kind) {
		Target.Kind = Kind;
		FArrayValueContainer Proposed{std::move(Values)};
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Array.get(), &Proposed, 0, Snapshot));
		Durin::FReflectedPropertyEditSession Session;
		EXPECT_TRUE(Session.Begin(Target, "Edit Array Structure", nullptr, &Transactions));
		EXPECT_EQ(Session.Apply(Snapshot), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Object.Changes.back().Kind, Kind);
	};

	CommitValues({4, 8, 15, 16}, Durin::EPropertyChangeKind::ArrayAdd);
	EXPECT_EQ(Container.Values.size(), 4u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{4, 8, 15}));
	Transactions.Clear();
	CommitValues({4, 8}, Durin::EPropertyChangeKind::ArrayRemove);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{4, 8, 15}));
	Transactions.Clear();
	CommitValues({4}, Durin::EPropertyChangeKind::ArrayResize);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values, (std::vector<Durin::int32>{4, 8, 15}));
}

TEST(FReflectedPropertyEditSessionTests, MapTransactionsPreserveStableKeyPathsAndStructuralKinds)
{
	Durin::FStringProperty KeyProperty(
		Durin::FFieldVariant(), Durin::FName("Key"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
		1, 0, static_cast<Durin::uint16>(sizeof(std::string)), Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
	);
	auto ValueProperty = MakeValueProperty();
	auto MapProperty = MakeMapProperty(KeyProperty, *ValueProperty);
	FMapValueContainer Container{{{"Alpha", 1}, {"Beta", 2}}};
	DEditObserver Object;
	Durin::FReflectedPropertyEditTarget MapTarget = Durin::FReflectedPropertyEditTarget::ForMember(&Object, MapProperty.get());
	MapTarget.SnapshotContainer = &Container;
	Durin::FPropertyValueSnapshot KeySnapshot;
	const std::string Alpha = "Alpha";
	ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &Alpha, 0, KeySnapshot));
	Durin::FReflectedPropertyEditTarget ValueTarget = MapTarget.ForMapEntry(
		ValueProperty.get(), KeySnapshot, KeySnapshot.GetBytes()
	);
	Durin::FEditorTransactionManager Transactions;
	Durin::FReflectedPropertyEditSession ValueSession;
	ASSERT_TRUE(ValueSession.Begin(ValueTarget, "Edit Map Value", nullptr, &Transactions));
	FMapValueContainer ValueProposal{{{"Alpha", 9}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot ValueSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &ValueProposal, 0, ValueSnapshot));
	ASSERT_EQ(ValueSession.Apply(ValueSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(ValueSession.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(Object.Changes.back().Selectors.size(), 2u);
	EXPECT_EQ(Object.Changes.back().Selectors[0], Durin::EPropertyPathSelector::MapKey);
	EXPECT_EQ(Object.Changes.back().MapKeyData, KeySnapshot.GetBytes());
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Container.Values.at("Alpha"), 1);
	Transactions.Clear();

	auto CommitMap = [&](FStringIntMap Values, Durin::EPropertyChangeKind Kind, const std::string& PathKey) {
		Durin::FPropertyValueSnapshot StableKey;
		ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &PathKey, 0, StableKey));
		Durin::FReflectedPropertyEditTarget Target = MapTarget;
		Target.Kind = Kind;
		Target.Path.back().Selector = Durin::EPropertyPathSelector::MapKey;
		Target.Path.back().MapKeyData = StableKey.GetBytes();
		FMapValueContainer Proposed{std::move(Values)};
		Durin::FPropertyValueSnapshot Snapshot;
		ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &Proposed, 0, Snapshot));
		Durin::FReflectedPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(Target, "Edit Map Structure", nullptr, &Transactions));
		ASSERT_EQ(Session.Apply(Snapshot), Durin::EReflectedPropertyEditResult::Changed);
		ASSERT_EQ(Session.Commit(), Durin::EReflectedPropertyEditResult::Changed);
		EXPECT_EQ(Object.Changes.back().Kind, Kind);
		EXPECT_EQ(Object.Changes.back().MapKeyData, StableKey.GetBytes());
	};

	CommitMap({{"Alpha", 1}, {"Beta", 2}, {"Gamma", 3}}, Durin::EPropertyChangeKind::MapInsert, "Gamma");
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FALSE(Container.Values.contains("Gamma"));
	Transactions.Clear();
	CommitMap({{"Beta", 2}}, Durin::EPropertyChangeKind::MapRemove, "Alpha");
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Container.Values.contains("Alpha"));
	Transactions.Clear();

	std::string EditedKey = "Alpha";
	Durin::FReflectedPropertyEditTarget RenameTarget = MapTarget.ForMapEntry(
		&KeyProperty, KeySnapshot, KeySnapshot.GetBytes());
	RenameTarget.Kind = Durin::EPropertyChangeKind::MapKeyRename;
	Durin::FReflectedPropertyEditSession RenameSession;
	ASSERT_TRUE(RenameSession.Begin(RenameTarget, "Rename Map Key", nullptr, &Transactions));
	FMapValueContainer FirstRename{{{"Renamed", 1}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot FirstRenameSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &FirstRename, 0, FirstRenameSnapshot));
	std::string RenameError;
	ASSERT_EQ(RenameSession.Apply(FirstRenameSnapshot, &RenameError), Durin::EReflectedPropertyEditResult::Changed) << RenameError;
	const std::string Renamed = "Renamed";
	Durin::FPropertyValueSnapshot RenamedKeySnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(&KeyProperty, &Renamed, 0, RenamedKeySnapshot));
	Durin::FReflectedPropertyEditTarget ContinuedRename = MapTarget.ForMapEntry(
		&KeyProperty, RenamedKeySnapshot, RenamedKeySnapshot.GetBytes());
	ContinuedRename.Kind = Durin::EPropertyChangeKind::MapKeyRename;
	EXPECT_TRUE(RenameSession.MatchesTarget(ContinuedRename));
	FMapValueContainer FinalRename{{{"Final", 1}, {"Beta", 2}}};
	Durin::FPropertyValueSnapshot FinalRenameSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(MapProperty.get(), &FinalRename, 0, FinalRenameSnapshot));
	ASSERT_EQ(RenameSession.Apply(FinalRenameSnapshot), Durin::EReflectedPropertyEditResult::Changed);
	ASSERT_EQ(RenameSession.Commit(), Durin::EReflectedPropertyEditResult::Changed);
	EXPECT_EQ(Transactions.ConsumeEvents().size(), 1u);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_TRUE(Container.Values.contains("Alpha"));
	EXPECT_FALSE(Container.Values.contains("Final"));
}
