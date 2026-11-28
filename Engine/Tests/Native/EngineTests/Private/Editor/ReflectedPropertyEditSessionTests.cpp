#include "NativePropertyControlTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
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
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true));
	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, ProposedSnapshot));
	EXPECT_TRUE(Session.Finish(false));
	EXPECT_EQ(Object->GuidValue, Proposed);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->GuidValue, Original);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->GuidValue, Proposed);
}

TEST(FReflectedPropertyEditSessionTests, CanceledPropertyScopeRetiresBeforeNextRecording)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	FManagedEditObserver Object;
	FTestTransactorOwner Transactions;
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 8)));
	Transactions.Get()->Cancel();
	Container.Value = 11;
	Durin::FScopedTransaction Replacement("Replacement");
	std::string Error;
	EXPECT_FALSE(Session.Finish(false, &Error));
	EXPECT_EQ(Error, "The property transaction is no longer active.");
	EXPECT_FALSE(Session.IsActive());
	ASSERT_TRUE(Replacement.IsOutstanding());
	EXPECT_EQ(Container.Value, 11);
	EXPECT_EQ(Object.Changes.size(), 1u);
}

TEST(FReflectedPropertyEditSessionTests, RetiredRecordingDiscardsLatePreviewInput)
{
	auto Property = MakeValueProperty();
	FTestTransactorOwner Transactions;
	for (bool bCancel : {false, true})
	{
		FValueContainer Container{5};
		FManagedEditObserver Object;
		Durin::PropertyEditor::FPropertyControl Session;
		ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), true));
		ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 8)));
		const auto Next = CaptureValue(Property.get(), Container, 9);
		Transactions.Get()->Cancel();
		EXPECT_FALSE(bCancel ? Session.Finish(true) : Durin::Testing::ApplyControlInput(Session, Next));
		EXPECT_FALSE(Session.IsActive());
		EXPECT_EQ(Container.Value, 8);
		EXPECT_EQ(Object.Changes.size(), 1u);
	}
}

TEST(FReflectedPropertyEditSessionTests, AppliesInteractiveValuesAndCommitsOnce)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	FManagedEditObserver Object;
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), false));
	const Durin::FPropertyValueSnapshot Proposed = CaptureValue(Property.get(), Container, 19);

	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, Proposed));
	EXPECT_EQ(Container.Value, 19);
	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, Proposed));
	ASSERT_EQ(Object.Changes.size(), 1u);
	EXPECT_EQ(Object.Changes[0].ChangeType, Durin::EPropertyChangeType::Interactive);
	EXPECT_TRUE(Session.Finish(false));
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
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Durin::FPropertyEditTarget::ForMember(Object, Property)), true));

	Object->PreChange = [](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		return std::unexpected(Durin::FObjectValidationError{.Code = Durin::EObjectValidationError::PropertyRejected, .PropertyReason = Durin::EPropertyEditRejection::Rejected});
	};
	std::string Error;
	Object->Value = 19;
	Durin::FPropertyValueSnapshot ProposedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, ProposedSnapshot));
	Object->Value = 7;
	const bool EditResult1 = Durin::Testing::ApplyControlInput(Session, ProposedSnapshot, &Error);
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
	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, ProposedSnapshot));
	EXPECT_EQ(Object->Value, 10);
	Durin::FPropertyValueSnapshot NormalizedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, NormalizedSnapshot));
	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, NormalizedSnapshot));
	EXPECT_EQ(Object->Changes.size(), 1u);
	EXPECT_TRUE(Session.Finish(false));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 7);
	EXPECT_EQ(Object->LastProposalFlags, Durin::PropertyEditor::EPropertyValueSetFlags::InteractiveChange | Durin::PropertyEditor::EPropertyValueSetFlags::NotTransactable);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 10);
}

TEST(FReflectedPropertyEditSessionTests, GenericHookRejectsNestedEditOfSameTarget)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{2};
	FManagedEditObserver Object;
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), false));
	const Durin::FPropertyValueSnapshot NestedProposal = CaptureValue(Property.get(), Container, 6);
	bool bNestedSucceeded = true;
	std::string NestedError;
	Object.PreChange = [&](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		const bool EditResult2 = Durin::Testing::ApplyControlInput(Session, NestedProposal, &NestedError);
		bNestedSucceeded = EditResult2;
		return {};
	};

	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 9)));
	EXPECT_FALSE(bNestedSucceeded);
	EXPECT_EQ(NestedError, "A reflected property hook cannot start a nested edit of the same target.");
	EXPECT_EQ(Container.Value, 9);
	EXPECT_TRUE(Session.Finish(true));
}

TEST(FReflectedPropertyEditSessionTests, GeneratesDefaultDescriptionOnlyForValidTargets)
{
	Durin::Testing::InitializeAssetSystemForTests();
	Durin::PropertyEditor::FPropertyControl Session;
	std::string Error;
	const bool EditResult3 = Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle({}), false, false, &Error);
	EXPECT_FALSE(EditResult3);
	EXPECT_EQ(Error, "The edit target has no owning object.");
	EXPECT_FALSE(Session.IsActive());

	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	FManagedEditObserver Object;
	Durin::FPropertyEditTarget Incomplete = MakeTarget(Object, Property.get(), Container);
	Incomplete.SnapshotProperty = nullptr;
	Incomplete.SnapshotContainer = nullptr;
	const bool EditResult4 = Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Incomplete), false, false, &Error);
	EXPECT_FALSE(EditResult4);
	EXPECT_EQ(Error, "The edit target is incomplete.");
	EXPECT_FALSE(Session.IsActive());

	FTestTransactorOwner Transactions;
	const bool EditResult5 = Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), true, false, &Error);
	ASSERT_TRUE(EditResult5) << Error;
	ASSERT_NE(Durin::GUndo, nullptr);
	EXPECT_EQ(Durin::GUndo->GetContext().Description, "Edit Value");
	EXPECT_TRUE(Session.Finish(true));
	EXPECT_TRUE(Object.Changes.empty());
}

TEST(FReflectedPropertyEditSessionTests, EndingDragKeepsLastValueAndOwnedPathData)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{3};
	FManagedEditObserver Object;
	Durin::FPropertyEditTarget Target = MakeTarget(Object, Property.get(), Container);
	Target.Path[0].Index = 3;
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), false));
	Target.Path[0].Index = 9;

	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 11)));
	EXPECT_TRUE(Session.Finish(true));
	EXPECT_EQ(Container.Value, 11);
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
	Durin::PropertyEditor::FPropertyControl Session;
	std::string Error;
	const bool EditResult6 = Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), false, false, &Error);
	ASSERT_TRUE(EditResult6) << Error;

	const bool EditResult7 = Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 8), &Error);
	EXPECT_FALSE(EditResult7);
	EXPECT_EQ(Container.Value, 5);
	EXPECT_TRUE(Object.Changes.empty());
	EXPECT_TRUE(Session.Finish(false));
	EXPECT_TRUE(Object.Changes.empty());
}

TEST(FReflectedPropertyEditSessionTests, EndingDragDoesNotValidateAnotherProposal)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	FManagedEditObserver Object;
	Durin::PropertyEditor::FPropertyControl Session;
	std::string Error;
	const bool EditResult8 = Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), false, false, &Error);
	ASSERT_TRUE(EditResult8) << Error;
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 8)));

	Object.PreChange = [](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		ADD_FAILURE() << "Restoring the original value must bypass proposal validation";
		return std::unexpected(Durin::FObjectValidationError{.Code = Durin::EObjectValidationError::PropertyRejected});
	};
	ASSERT_TRUE(Session.Finish(true, &Error)) << Error;
	EXPECT_FALSE(Session.IsActive());
	EXPECT_EQ(Container.Value, 8);
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
	PropertyEditor::FPropertyControl Session;
	FTestTransactorOwner Transactions;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true, true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(&Property, Container, 8)));
	const auto FailRead = [](FArchive& Ar, void* Storage) {
		if (Ar.IsLoading()) Ar.SetError("Original value cannot be decoded");
		else Ar << *static_cast<int32*>(Storage);
	};
	Property.SetBulkDataOperations(FailRead, nullptr);
	std::string Error;
	EXPECT_FALSE(Session.Finish(true, &Error));
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
	EXPECT_TRUE(Reports.empty()); // Finishing an ordinary drag does not deserialize a rollback snapshot.
}

TEST(FReflectedPropertyEditSessionTests, EmitsTerminalEventAfterReturningToOriginalValue)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{5};
	FManagedEditObserver Object;
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), false));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 8)));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 5)));

	EXPECT_TRUE(Session.Finish(false));
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
		Durin::PropertyEditor::FPropertyControl Session;
		ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), false));
		EXPECT_TRUE(Session.Finish(false));
		EXPECT_TRUE(Object.Changes.empty());
	}

	{
		Durin::PropertyEditor::FPropertyControl Session;
		ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(MakeTarget(Object, Property.get(), Container)), false));
		EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, CaptureValue(Property.get(), Container, 27)));
		EXPECT_EQ(Container.Value, 27);
	}
	EXPECT_EQ(Container.Value, 27);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[0].ChangeType, Durin::EPropertyChangeType::Interactive);
	EXPECT_EQ(Object.Changes[1].ChangeType, Durin::EPropertyChangeType::ValueSet);
}
