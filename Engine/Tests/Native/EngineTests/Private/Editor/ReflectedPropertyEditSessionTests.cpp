#include "NativeAssetRuntimeTestSupport.h"
#include "PropertyEditor/PropertyHandle.h"
#include "PropertyEditor/PropertyView.h"
#include "ReflectedPropertyEditingTestSupport.h"
#include "NativeDObjectTestSupport.h"

TEST(FReflectedPropertyEditSessionTests, CommitsAndUndoRedoesGuidValues)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "GuidTransactionTarget");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(Object);
	auto* Property = DReflectedTransactionTestObject::FindProperty("GuidValue");
	const Durin::FGuid Original(1, 2, 3, 4);
	const Durin::FGuid Proposed(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
	Object->GuidValue = Proposed;
	Durin::FPropertyValueSnapshot ProposedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, ProposedSnapshot));
	Object->GuidValue = Original;
	const Durin::FPropertyEditTarget Target =
		Durin::FPropertyEditTarget::ForMember(Object, Property);
	FTestTransactorOwner Transactions;
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Guid", *Transactions.Get()));
	EXPECT_TRUE(Session.Apply(ProposedSnapshot));
	EXPECT_TRUE(Session.Commit());
	EXPECT_EQ(Object->GuidValue, Proposed);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->GuidValue, Original);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->GuidValue, Proposed);
}

TEST(FReflectedPropertyEditSessionTests, StaleCommitLeavesReplacementRecordingAndLaterValueIntact)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	FManagedEditObserver Object;
	FTestTransactorOwner Transactions;
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Old edit", *Transactions.Get()));
	ASSERT_TRUE(Session.Apply(CaptureValue(Property.get(), Container, 8)));
	Transactions.Get()->Cancel();
	Durin::FScopedTransaction Replacement("Replacement");
	ASSERT_TRUE(Replacement.IsRecordingCurrent());
	Container.Value = 11;
	std::string Error;
	EXPECT_FALSE(Session.Commit(&Error));
	EXPECT_EQ(Error, "The property transaction is no longer active.");
	EXPECT_FALSE(Session.IsActive());
	EXPECT_TRUE(Replacement.IsRecordingCurrent());
	EXPECT_EQ(Container.Value, 11);
	EXPECT_EQ(Object.Changes.size(), 1u);
}

TEST(FReflectedPropertyEditSessionTests, AppliesInteractiveValuesAndCommitsOnce)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	FManagedEditObserver Object;
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", false));
	const Durin::FPropertyValueSnapshot Proposed = CaptureValue(Property.get(), Container, 19);

	EXPECT_TRUE(Session.Apply(Proposed));
	EXPECT_EQ(Container.Value, 19);
	EXPECT_TRUE(Session.Apply(Proposed));
	ASSERT_EQ(Object.Changes.size(), 1u);
	EXPECT_EQ(Object.Changes[0].ChangeType, Durin::EPropertyChangeType::Interactive);
	EXPECT_TRUE(Session.Commit());
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[1].ChangeType, Durin::EPropertyChangeType::ValueSet);
	EXPECT_FALSE(Session.IsActive());
}

TEST(FReflectedPropertyEditSessionTests, GenericHookRejectsAndNormalizesDetachedProposalsAtomically)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "ValidatedTransactionTarget");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(Object);
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 7;
	FTestTransactorOwner Transactions;
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(Durin::FPropertyEditTarget::ForMember(Object, Property), "Validated Edit", *Transactions.Get()));

	Object->PreChange = [](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		return std::unexpected(Durin::FObjectValidationError{.Code = Durin::EObjectValidationError::PropertyRejected, .PropertyReason = Durin::EPropertyEditRejection::Rejected});
	};
	std::string Error;
	Object->Value = 19;
	Durin::FPropertyValueSnapshot ProposedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, ProposedSnapshot));
	Object->Value = 7;
	const bool EditResult1 = Session.Apply(ProposedSnapshot, &Error);
	EXPECT_FALSE(EditResult1);
	EXPECT_EQ(Error, "The object rejected the reflected property proposal.");
	EXPECT_EQ(Object->Value, 7);
	EXPECT_TRUE(Object->Changes.empty());
	EXPECT_FALSE(Transactions.Get()->CanUndo());

	Object->PreChange = [Property](Durin::FPropertyEditProposal& Proposal) -> std::expected<void, Durin::FObjectValidationError> {
		auto* Value = Property->ContainerPtrToValuePtr<int32>(Proposal.DraftLeafContainer, Proposal.DraftLeafArrayIndex);
		*Value = std::clamp(*Value, 0, 10);
		return {};
	};
	EXPECT_TRUE(Session.Apply(ProposedSnapshot));
	EXPECT_EQ(Object->Value, 10);
	Durin::FPropertyValueSnapshot NormalizedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, NormalizedSnapshot));
	EXPECT_EQ(Session.GetCurrentValue(), NormalizedSnapshot.GetPayload());
	EXPECT_TRUE(Session.Commit());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 7);
	EXPECT_EQ(Object->LastProposalFlags, Durin::PropertyEditor::EPropertyValueSetFlags::InteractiveChange);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 10);
}

TEST(FReflectedPropertyEditSessionTests, GenericHookRejectsNestedEditOfSameTarget)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{2};
	FManagedEditObserver Object;
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Reentrant Edit", false));
	const Durin::FPropertyValueSnapshot NestedProposal = CaptureValue(Property.get(), Container, 6);
	bool bNestedSucceeded = true;
	std::string NestedError;
	Object.PreChange = [&](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		const bool EditResult2 = Session.Apply(NestedProposal, &NestedError);
		bNestedSucceeded = EditResult2;
		return {};
	};

	EXPECT_TRUE(Session.Apply(CaptureValue(Property.get(), Container, 9)));
	EXPECT_FALSE(bNestedSucceeded);
	EXPECT_EQ(NestedError, "A reflected property hook cannot start a nested edit of the same target.");
	EXPECT_EQ(Container.Value, 9);
	EXPECT_TRUE(Session.Cancel());
}

TEST(FReflectedPropertyEditSessionTests, GeneratesDefaultDescriptionOnlyForValidTargets)
{
	Durin::Testing::InitializeAssetSystemForTests();
	Durin::PropertyEditor::FPropertyEdit Session;
	std::string Error;
	const bool EditResult3 = Session.Begin({}, {}, false, &Error);
	EXPECT_FALSE(EditResult3);
	EXPECT_EQ(Error, "The edit target has no owning object.");
	EXPECT_FALSE(Session.IsActive());

	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	FManagedEditObserver Object;
	Durin::FPropertyEditTarget Incomplete = MakeTarget(Object, Property.get(), Container);
	Incomplete.SnapshotProperty = nullptr;
	Incomplete.SnapshotContainer = nullptr;
	const bool EditResult4 = Session.Begin(Incomplete, {}, false, &Error);
	EXPECT_FALSE(EditResult4);
	EXPECT_EQ(Error, "The edit target is incomplete.");
	EXPECT_FALSE(Session.IsActive());

	const bool EditResult5 = Session.Begin(MakeTarget(Object, Property.get(), Container), {}, false, &Error);
	ASSERT_TRUE(EditResult5) << Error;
	EXPECT_EQ(Session.GetDescription(), "Edit Value");
	EXPECT_TRUE(Session.Cancel());
	EXPECT_TRUE(Object.Changes.empty());
}

TEST(FReflectedPropertyEditSessionTests, CancelRestoresOriginalValueAndOwnedPathData)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{3};
	FManagedEditObserver Object;
	Durin::FPropertyEditTarget Target = MakeTarget(Object, Property.get(), Container);
	Target.Path[0].Index = 3;
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Nested Value", false));
	Target.Path[0].Index = 9;

	EXPECT_TRUE(Session.Apply(CaptureValue(Property.get(), Container, 11)));
	EXPECT_TRUE(Session.Cancel());
	EXPECT_EQ(Container.Value, 3);
	ASSERT_EQ(Object.Changes.size(), 2u);
	ASSERT_EQ(Object.Changes[0].Indices.size(), 1u);
	EXPECT_EQ(Object.Changes[0].Indices[0], 3u);
	EXPECT_EQ(Object.Changes[1].ChangeType, Durin::EPropertyChangeType::ValueSet);
}

TEST(FReflectedPropertyEditSessionTests, RejectsMutationWithoutChangingOrNotifying)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	FManagedEditObserver Object;
	Object.PreChange = [](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		return std::unexpected(Durin::FObjectValidationError{.Code = Durin::EObjectValidationError::PropertyRejected, .PropertyReason = Durin::EPropertyEditRejection::Rejected});
	};
	Durin::PropertyEditor::FPropertyEdit Session;
	std::string Error;
	const bool EditResult6 = Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", false, &Error);
	ASSERT_TRUE(EditResult6) << Error;

	const bool EditResult7 = Session.Apply(CaptureValue(Property.get(), Container, 8), &Error);
	EXPECT_FALSE(EditResult7);
	EXPECT_EQ(Container.Value, 5);
	EXPECT_TRUE(Object.Changes.empty());
	EXPECT_TRUE(Session.Commit());
	EXPECT_TRUE(Object.Changes.empty());
}

TEST(FReflectedPropertyEditSessionTests, CancelRestoresOriginalWithoutValidatingANewProposal)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	FManagedEditObserver Object;
	Durin::PropertyEditor::FPropertyEdit Session;
	std::string Error;
	const bool EditResult8 = Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", false, &Error);
	ASSERT_TRUE(EditResult8) << Error;
	ASSERT_TRUE(Session.Apply(CaptureValue(Property.get(), Container, 8)));

	Object.PreChange = [](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		ADD_FAILURE() << "Restoring the original value must bypass proposal validation";
		return std::unexpected(Durin::FObjectValidationError{.Code = Durin::EObjectValidationError::PropertyRejected});
	};
	ASSERT_TRUE(Session.Cancel(&Error)) << Error;
	EXPECT_FALSE(Session.IsActive());
	EXPECT_EQ(Container.Value, 5);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[0].ChangeType, Durin::EPropertyChangeType::Interactive);
	EXPECT_EQ(Object.Changes[1].ChangeType, Durin::EPropertyChangeType::ValueSet);
}

TEST(FReflectedPropertyEditSessionTests, RestoreFailureClosesTheGestureWithItsDiagnostic)
{
	using namespace Durin;
	FManagedEditObserver Object;
	FValueContainer Container{5};
	FProperty Property({}, "Value", EObjectFlags::NoFlags, EPropertyFlags::Edit, 1,
		offsetof(FValueContainer, Value), sizeof(int32), DurinCodeGen::EPropertyGenFlags::BulkData, nullptr);
	SetTestValueLifecycle<int32>(Property);
	const auto Serialize = [](FArchive& Ar, void* Storage) { Ar << *static_cast<int32*>(Storage); };
	Property.SetBulkDataOperations(Serialize, nullptr);
	const auto Target = MakeTarget(Object, &Property, Container);
	PropertyEditor::FPropertyEdit Session;
	FTestTransactorOwner Transactions;
	ASSERT_TRUE(Session.Begin(Target, "Preview", *Transactions.Get()));
	ASSERT_TRUE(Session.Apply(CaptureValue(&Property, Container, 8)));
	const auto FailRead = [](FArchive& Ar, void* Storage) {
		if (Ar.IsLoading()) Ar.SetError("Original value cannot be decoded");
		else Ar << *static_cast<int32*>(Storage);
	};
	Property.SetBulkDataOperations(FailRead, nullptr);
	std::string Error;
	EXPECT_FALSE(Session.Cancel(&Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_FALSE(Session.IsActive());
	EXPECT_EQ(Container.Value, 8);
	EXPECT_EQ(Object.Changes.size(), 1u);
	EXPECT_FALSE(Transactions.Get()->IsActive());
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	Property.SetBulkDataOperations(Serialize, nullptr);
	FPropertyView View;
	std::vector<std::string> Reports;
	const FPropertyViewContext Context{.ReportPropertyError = [&](std::string Message) { Reports.push_back(std::move(Message)); }};
	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, Target, [](FProperty* Field, void* Data, uint32 Index) {
		*Field->ContainerPtrToValuePtr<int32>(Data, Index) = 9;
	}, true));
	Property.SetBulkDataOperations(FailRead, nullptr);
	FManagedEditObserver NextObject;
	EXPECT_TRUE(View.HandleOwnerContext(Context, NextObject.Get()));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(Container.Value, 9);
	EXPECT_FALSE(Transactions.Get()->IsActive());
	EXPECT_TRUE(View.HandleOwnerContext(Context, NextObject.Get()));
	ASSERT_EQ(Reports.size(), 1u);
	EXPECT_EQ(Reports.front(), Error);
}

TEST(FReflectedPropertyEditSessionTests, EmitsTerminalEventAfterReturningToOriginalValue)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	FManagedEditObserver Object;
	Durin::PropertyEditor::FPropertyEdit Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", false));
	ASSERT_TRUE(Session.Apply(CaptureValue(Property.get(), Container, 8)));
	ASSERT_TRUE(Session.Apply(CaptureValue(Property.get(), Container, 5)));

	EXPECT_TRUE(Session.Commit());
	ASSERT_EQ(Object.Changes.size(), 3u);
	EXPECT_EQ(Object.Changes[0].ChangeType, Durin::EPropertyChangeType::Interactive);
	EXPECT_EQ(Object.Changes[1].ChangeType, Durin::EPropertyChangeType::Interactive);
	EXPECT_EQ(Object.Changes[2].ChangeType, Durin::EPropertyChangeType::ValueSet);
}

TEST(FReflectedPropertyEditSessionTests, NoOpCommitAndSessionDestructionDoNotAbandonPreviewState)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{13};
	FManagedEditObserver Object;
	{
		Durin::PropertyEditor::FPropertyEdit Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "No-op Edit", false));
		EXPECT_TRUE(Session.Commit());
		EXPECT_TRUE(Object.Changes.empty());
	}

	{
		Durin::PropertyEditor::FPropertyEdit Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Abandoned Preview", false));
		EXPECT_TRUE(Session.Apply(CaptureValue(Property.get(), Container, 27)));
		EXPECT_EQ(Container.Value, 27);
	}
	EXPECT_EQ(Container.Value, 13);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[0].ChangeType, Durin::EPropertyChangeType::Interactive);
	EXPECT_EQ(Object.Changes[1].ChangeType, Durin::EPropertyChangeType::ValueSet);
}
