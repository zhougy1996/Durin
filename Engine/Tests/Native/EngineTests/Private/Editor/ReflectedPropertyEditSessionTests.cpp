#include "ReflectedPropertyEditingTestSupport.h"

TEST(FReflectedPropertyEditSessionTests, CommitsAndUndoRedoesGuidValues)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "GuidTransactionTarget");
	Durin::FScopedObjectRoot ObjectRoot(Object);
	auto* Property = DReflectedTransactionTestObject::FindProperty("GuidValue");
	const Durin::FGuid Original(1, 2, 3, 4);
	const Durin::FGuid Proposed(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
	Object->GuidValue = Proposed;
	Durin::FPropertyValueSnapshot ProposedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, ProposedSnapshot));
	Object->GuidValue = Original;
	const Durin::Editor::FPropertyEditTarget Target =
		Durin::Editor::FPropertyEditTarget::ForMember(Object, Property);
	FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Guid", nullptr, Transactions.Get()));
	EXPECT_EQ(Session.Apply(ProposedSnapshot), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Object->GuidValue, Proposed);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->GuidValue, Original);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->GuidValue, Proposed);
}

TEST(FReflectedPropertyEditSessionTests, AppliesInteractiveValuesAndCommitsOnce)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	DEditObserver Object;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value"));
	const Durin::FPropertyValueSnapshot Proposed = CaptureValue(Property.get(), Container, 19);

	EXPECT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Container.Value, 19);
	EXPECT_EQ(Session.Apply(Proposed), Durin::Editor::EPropertyEditResult::NoChange);
	ASSERT_EQ(Object.Changes.size(), 1u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Object.Changes.size(), 2u);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Committed);
	EXPECT_FALSE(Session.IsActive());
}

TEST(FReflectedPropertyEditSessionTests, GenericHookRejectsAndNormalizesDetachedProposalsAtomically)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "ValidatedTransactionTarget");
	Durin::FScopedObjectRoot ObjectRoot(Object);
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 7;
	FTestTransactorOwner Transactions;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Durin::Editor::FPropertyEditTarget::ForMember(Object, Property), "Validated Edit", nullptr,
		Transactions.Get()));

	Object->PreChange = [](Durin::FPropertyEditProposal&, std::string& Error) {
		Error = "Rejected detached proposal.";
		return false;
	};
	std::string Error;
	Object->Value = 19;
	Durin::FPropertyValueSnapshot ProposedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, ProposedSnapshot));
	Object->Value = 7;
	EXPECT_EQ(Session.Apply(ProposedSnapshot, &Error), Durin::Editor::EPropertyEditResult::Failed);
	EXPECT_EQ(Error, "Rejected detached proposal.");
	EXPECT_EQ(Object->Value, 7);
	EXPECT_TRUE(Object->Changes.empty());
	EXPECT_FALSE(Transactions.Get()->CanUndo());

	Object->PreChange = [Property](Durin::FPropertyEditProposal& Proposal, std::string&) {
		auto* Value = Property->ContainerPtrToValuePtr<int32>(Proposal.DraftLeafContainer, Proposal.DraftLeafArrayIndex);
		*Value = std::clamp(*Value, 0, 10);
		return true;
	};
	EXPECT_EQ(Session.Apply(ProposedSnapshot), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Object->Value, 10);
	Durin::FPropertyValueSnapshot NormalizedSnapshot;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, NormalizedSnapshot));
	EXPECT_EQ(Session.GetCurrentValue(), NormalizedSnapshot.GetPayload());
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 7);
	EXPECT_EQ(Object->LastProposalPhase, Durin::EPropertyChangePhase::Committed);
	EXPECT_EQ(Object->LastProposalOrigin, Durin::EPropertyChangeOrigin::Undo);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 10);
}

TEST(FReflectedPropertyEditSessionTests, GenericHookRejectsNestedEditOfSameTarget)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{2};
	DEditObserver Object;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Reentrant Edit"));
	const Durin::FPropertyValueSnapshot NestedProposal = CaptureValue(Property.get(), Container, 6);
	Durin::Editor::EPropertyEditResult NestedResult = Durin::Editor::EPropertyEditResult::Changed;
	std::string NestedError;
	Object.PreChange = [&](Durin::FPropertyEditProposal&, std::string&) {
		NestedResult = Session.Apply(NestedProposal, &NestedError);
		return true;
	};

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 9)), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(NestedResult, Durin::Editor::EPropertyEditResult::Failed);
	EXPECT_EQ(NestedError, "A reflected property hook cannot start a nested edit of the same target.");
	EXPECT_EQ(Container.Value, 9);
	EXPECT_EQ(Session.Cancel(), Durin::Editor::EPropertyEditResult::Changed);
}

TEST(FReflectedPropertyEditSessionTests, GeneratesDefaultDescriptionOnlyForValidTargets)
{
	Durin::Editor::FPropertyEditSession Session;
	std::string Error;
	EXPECT_FALSE(Session.Begin({}, {}, &Error));
	EXPECT_EQ(Error, "The edit target has no owning object.");
	EXPECT_FALSE(Session.IsActive());

	auto Property = MakeValueProperty();
	FValueContainer Container{7};
	DEditObserver Object;
	Durin::Editor::FPropertyEditTarget Incomplete = MakeTarget(Object, Property.get(), Container);
	Incomplete.SnapshotProperty = nullptr;
	Incomplete.SnapshotContainer = nullptr;
	EXPECT_FALSE(Session.Begin(Incomplete, {}, &Error));
	EXPECT_EQ(Error, "The edit target is incomplete.");
	EXPECT_FALSE(Session.IsActive());

	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), {}, &Error)) << Error;
	EXPECT_EQ(Session.GetDescription(), "Edit Value");
	EXPECT_EQ(Session.Cancel(), Durin::Editor::EPropertyEditResult::NoChange);
	ASSERT_EQ(Object.Changes.size(), 1u);
	EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Cancelled);
}

TEST(FReflectedPropertyEditSessionTests, CancelRestoresOriginalValueAndOwnedPathData)
{
	auto Property = MakeValueProperty();
	FValueContainer Container{3};
	DEditObserver Object;
	Durin::Editor::FPropertyEditTarget Target = MakeTarget(Object, Property.get(), Container);
	Target.Path[0].Index = 3;
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(Target, "Edit Nested Value"));
	Target.Path[0].Index = 9;

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 11)), Durin::Editor::EPropertyEditResult::Changed);
	EXPECT_EQ(Session.Cancel(), Durin::Editor::EPropertyEditResult::Changed);
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
	Durin::Editor::FPropertyEditSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", &Error)) << Error;

	EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8), &Error), Durin::Editor::EPropertyEditResult::Failed);
	EXPECT_EQ(Container.Value, 5);
	EXPECT_TRUE(Object.Changes.empty());
	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::NoChange);
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
	Durin::Editor::FPropertyEditSession Session;
	std::string Error;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value", &Error)) << Error;
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8)), Durin::Editor::EPropertyEditResult::Changed);

	EXPECT_EQ(Session.Cancel(&Error), Durin::Editor::EPropertyEditResult::Failed);
	EXPECT_EQ(Error, "Restore rejected for testing.");
	EXPECT_TRUE(Session.IsActive());
	EXPECT_EQ(Container.Value, 8);
	bAllowRestore = true;
	EXPECT_EQ(Session.Cancel(&Error), Durin::Editor::EPropertyEditResult::Changed);
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
	Durin::Editor::FPropertyEditSession Session;
	ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Edit Value"));
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 8)), Durin::Editor::EPropertyEditResult::Changed);
	ASSERT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 5)), Durin::Editor::EPropertyEditResult::Changed);

	EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::NoChange);
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
		Durin::Editor::FPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "No-op Edit"));
		EXPECT_EQ(Session.Commit(), Durin::Editor::EPropertyEditResult::NoChange);
		ASSERT_EQ(Object.Changes.size(), 1u);
		EXPECT_EQ(Object.Changes[0].Phase, Durin::EPropertyChangePhase::Committed);
	}

	{
		Durin::Editor::FPropertyEditSession Session;
		ASSERT_TRUE(Session.Begin(MakeTarget(Object, Property.get(), Container), "Abandoned Preview"));
		EXPECT_EQ(Session.Apply(CaptureValue(Property.get(), Container, 27)), Durin::Editor::EPropertyEditResult::Changed);
		EXPECT_EQ(Container.Value, 27);
	}
	EXPECT_EQ(Container.Value, 13);
	ASSERT_EQ(Object.Changes.size(), 3u);
	EXPECT_EQ(Object.Changes[1].Phase, Durin::EPropertyChangePhase::Interactive);
	EXPECT_EQ(Object.Changes[2].Phase, Durin::EPropertyChangePhase::Cancelled);
}
