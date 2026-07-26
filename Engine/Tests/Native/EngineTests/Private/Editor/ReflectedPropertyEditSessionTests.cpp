#include "ReflectedPropertyEditingTestSupport.h"

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
