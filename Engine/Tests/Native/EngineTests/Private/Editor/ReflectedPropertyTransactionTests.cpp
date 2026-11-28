#include "NativePropertyControlTestSupport.h"
#include "ReflectedPropertyEditingTestSupport.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Editor/EditorTransactionTestSupport.h"

TEST(FReflectedPropertyEditSessionTests, PostEditUndoDefaultsToFullObjectChange)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "DefaultPostEditUndo");
	Object->Durin::DObject::PostEditUndo();
	ASSERT_EQ(Object->Changes.size(), 1u);
	EXPECT_EQ(Object->Changes.front().MemberProperty, nullptr);
	EXPECT_EQ(Object->Changes.front().LeafProperty, nullptr);
	Durin::MarkObjectHierarchyAsGarbage(Object);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FReflectedPropertyEditSessionTests, PostEditUndoRunsOncePerObjectAfterAllFieldsAreRestored)
{
	InitializeDObjectSystem();
	auto* First = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "FirstPostEditUndo");
	auto* Second = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "SecondPostEditUndo");
	FTestTransactorOwner Transactions;
	const Durin::FGuid ChangedGuid = Durin::FGuid::NewGuid();
	{
		Durin::FScopedTransaction Transaction("test", "Change multiple objects", nullptr);
		(void)First->Modify();
		(void)Second->Modify();
		First->Value = 11;
		First->GuidValue = ChangedGuid;
		Second->Value = 22;
	}
	EXPECT_EQ(First->PostUndoCount, 0u);
	EXPECT_EQ(Second->PostUndoCount, 0u);
	EXPECT_EQ(First->PreUndoCount, 0u);
	EXPECT_EQ(Second->PreUndoCount, 0u);
	auto ExpectBeforeUndo = [&] {
		EXPECT_EQ(First->Value, 11);
		EXPECT_EQ(First->GuidValue, ChangedGuid);
		EXPECT_EQ(Second->Value, 22);
	};
	First->PreUndo = ExpectBeforeUndo;
	Second->PreUndo = ExpectBeforeUndo;
	auto ExpectRestored = [&] {
		EXPECT_EQ(First->Value, 0);
		EXPECT_EQ(First->GuidValue, Durin::FGuid{});
		EXPECT_EQ(Second->Value, 0);
	};
	First->PostUndo = ExpectRestored;
	Second->PostUndo = ExpectRestored;
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(First->PostUndoCount, 1u);
	EXPECT_EQ(Second->PostUndoCount, 1u);
	EXPECT_EQ(First->PreUndoCount, 1u);
	EXPECT_EQ(Second->PreUndoCount, 1u);
	auto ExpectReapplied = [&] {
		EXPECT_EQ(First->Value, 11);
		EXPECT_EQ(First->GuidValue, ChangedGuid);
		EXPECT_EQ(Second->Value, 22);
	};
	First->PostUndo = ExpectReapplied;
	Second->PostUndo = ExpectReapplied;
	First->PreUndo = ExpectRestored;
	Second->PreUndo = ExpectRestored;
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(First->PostUndoCount, 2u);
	EXPECT_EQ(Second->PostUndoCount, 2u);
	EXPECT_EQ(First->PreUndoCount, 2u);
	EXPECT_EQ(Second->PreUndoCount, 2u);
	Transactions.Get()->Reset();
	Durin::MarkObjectHierarchyAsGarbage(First);
	Durin::MarkObjectHierarchyAsGarbage(Second);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FReflectedPropertyEditSessionTests, PostEditUndoSeesRestoredStateDespiteNewEditRejection)
{
	InitializeDObjectSystem();
	auto* First = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "RejectedPostEditUndo");
	auto* Second = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "CompensatedPostEditUndo");
	FTestTransactorOwner Transactions;
	{
		Durin::FScopedTransaction Transaction("test", "Rejected undo", nullptr);
		(void)First->Modify();
		(void)Second->Modify();
		First->Value = 11;
		Second->Value = 22;
	}
	First->PreChange = [](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		return std::unexpected(Durin::FObjectValidationError{
			.Code = Durin::EObjectValidationError::PropertyRejected,
			.PropertyReason = Durin::EPropertyEditRejection::Rejected});
	};
	auto ExpectRestored = [&] {
		EXPECT_EQ(First->Value, 0);
		EXPECT_EQ(Second->Value, 0);
	};
	First->PostUndo = ExpectRestored;
	Second->PostUndo = ExpectRestored;
	EXPECT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(First->PostUndoCount, 1u);
	EXPECT_EQ(Second->PostUndoCount, 1u);
	ExpectRestored();
	EXPECT_TRUE(Transactions.Get()->CanRedo());
	Transactions.Get()->Reset();
	Durin::MarkObjectHierarchyAsGarbage(First);
	Durin::MarkObjectHierarchyAsGarbage(Second);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FReflectedPropertyEditSessionTests, ReplayBypassesProposalValidationForEveryTarget)
{
	InitializeDObjectSystem();
	auto* First = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "UnreachedReplayTarget");
	auto* Second = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "RejectingReplayTarget");
	FTestTransactorOwner Transactions;
	{
		Durin::FScopedTransaction Transaction("test", "Reject first replay record", nullptr);
		(void)First->Modify();
		(void)Second->Modify();
		First->Value = 11;
		Second->Value = 22;
	}
	Second->PreChange = [](Durin::FPropertyEditProposal&) -> std::expected<void, Durin::FObjectValidationError> {
		return std::unexpected(Durin::FObjectValidationError{
			.Code = Durin::EObjectValidationError::PropertyRejected,
			.PropertyReason = Durin::EPropertyEditRejection::Rejected});
	};
	EXPECT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(First->Value, 0);
	EXPECT_EQ(Second->Value, 0);
	EXPECT_EQ(First->PreChangeCount, 0u);
	EXPECT_EQ(First->PreUndoCount, 1u);
	EXPECT_EQ(First->PostUndoCount, 1u);
	EXPECT_EQ(Second->PreUndoCount, 1u);
	EXPECT_EQ(Second->PostUndoCount, 1u);
	Second->PreChange = {};
	ASSERT_TRUE(Transactions.Get()->Redo());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(First->Value, 0);
	EXPECT_EQ(Second->Value, 0);
	EXPECT_EQ(First->PreUndoCount, 3u);
	EXPECT_EQ(First->PostUndoCount, 3u);
	Transactions.Get()->Reset();
	Durin::MarkObjectHierarchyAsGarbage(First);
	Durin::MarkObjectHierarchyAsGarbage(Second);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST(FReflectedPropertyEditSessionTests, KeepsTargetAliveForTheSession)
{
	InitializeDObjectSystem();
	auto Property = MakeValueProperty();
	FValueContainer Container{4};
	Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PropertyEditTarget"));
	Durin::FPropertyEditTarget Target;
	Target.Object = Object;
	Target.MemberProperty = Property.get();
	Target.LeafProperty = Property.get();
	Target.SnapshotProperty = Property.get();
	Target.SnapshotContainer = &Container;
	Target.Path.push_back({Property.get()});

	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), false));
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Object));
	ASSERT_TRUE(Session.Finish(true));
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Object));
}

TEST(FReflectedPropertyEditSessionTests, RejectsACollectedTargetBeforeReadingItsSnapshotStorage)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "CollectedPropertyEditTarget");
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	const Durin::FPropertyEditTarget Target =
		Durin::FPropertyEditTarget::ForMember(Object, Property);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);

	Durin::PropertyEditor::FPropertyControl Session;
	std::string Error;
	const bool EditResult1 = Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), false, false, &Error);
	EXPECT_FALSE(EditResult1);
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
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Durin::FPropertyEditTarget::ForMember(Object, Property)), true));

	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, Capture(11)));
	EXPECT_TRUE(Durin::Testing::ApplyControlInput(Session, Capture(19)));
	EXPECT_TRUE(Session.Finish(false));
	EXPECT_TRUE(Transactions.Get()->CanUndo());
	EXPECT_FALSE(Transactions.Get()->CanRedo());
	EXPECT_EQ(Transactions.Get()->GetUndoContext(false).Description, "Edit Value");

	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 7);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
	EXPECT_TRUE(Transactions.Get()->CanRedo());
	ASSERT_EQ(Object->Changes.size(), 4u);
	EXPECT_EQ(Object->Changes[2].ChangeType, Durin::EPropertyChangeType::ValueSet);
	EXPECT_EQ(Object->Changes[2].Origin, Durin::EPropertyChangeOrigin::Edit);
	EXPECT_EQ(Object->Changes[3].ChangeType, Durin::EPropertyChangeType::ValueSet);
	EXPECT_EQ(Object->Changes[3].Origin, Durin::EPropertyChangeOrigin::Undo);

	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 19);
	EXPECT_TRUE(Transactions.Get()->CanUndo());
	EXPECT_FALSE(Transactions.Get()->CanRedo());
	ASSERT_EQ(Object->Changes.size(), 5u);
	EXPECT_EQ(Object->Changes[4].Origin, Durin::EPropertyChangeOrigin::Redo);
}

TEST(FReflectedPropertyEditSessionTests, SynchronizesPackageDirtyStateAtSavedRevision)
{
	Durin::DPackage* Package = MakeReflectedRevisionTestPackage();
	Durin::Testing::FScopedMountRegistryFixture MountFixture;
	Durin::Testing::RegisterMountPointForTests("/ReflectedRevisionTests/", Durin::Testing::GetTestWorkDirectory().generic_string());
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(Package, "PropertyEditTarget");
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 7;
	const Durin::FPropertyEditTarget Target =
		Durin::FPropertyEditTarget::ForMember(Object, Property);
	Object->Value = 19;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, Proposed));
	Object->Value = 7;
	FTestTransactorOwner Transactions;
	Package->ClearDirty();

	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, Proposed));
	ASSERT_TRUE(Session.Finish(false));
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 7);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 19);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Durin::Tests::SaveTransactionTestPackage(*Package));
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_FALSE(Package->IsDirty());
}

TEST(FReflectedPropertyEditSessionTests, DirtyStartsAtValidatedWriteAndSurvivesGestureRestoration)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	auto* Package = MakeReflectedRevisionTestPackage();
	auto* Object = NewObject<DReflectedTransactionTestObject>(Package, "GestureTarget");
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	const auto Target = FPropertyEditTarget::ForMember(Object, Property);
	FTestTransactorOwner Transactions;
	Object->Value = 7;

	for (bool bCancel : {false, true})
	{
		Package->ClearDirty();
		FPropertyControl Edit;
		ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true));
		ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, int32{7}));
		EXPECT_FALSE(Package->IsDirty());
		ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, int32{19}));
		EXPECT_TRUE(Package->IsDirty());
		if (bCancel)
		{
			ASSERT_TRUE(Edit.Finish(true));
			EXPECT_EQ(Object->Value, 19);
			ASSERT_TRUE(Transactions.Get()->Undo());
			EXPECT_EQ(Object->Value, 7);
			continue;
		}
		else
		{
			ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, int32{7}));
			ASSERT_TRUE(Edit.Finish(false));
		}
		EXPECT_EQ(Object->Value, 7);
		EXPECT_TRUE(Package->IsDirty());
		EXPECT_FALSE(Transactions.Get()->CanUndo());
	}

	Package->ClearDirty();
	const auto Handle = MakePropertyHandle(Target);
	Object->PreChange = [](FPropertyEditProposal&) -> std::expected<void, FObjectValidationError> {
		return std::unexpected(FObjectValidationError{.Code = EObjectValidationError::PropertyRejected,
			.PropertyReason = EPropertyEditRejection::Rejected});
	};
	Object->Changes.clear();
	EXPECT_EQ(Handle->SetValue(19), FPropertyAccess::Fail);
	EXPECT_EQ(Handle->GetLastError(), "The object rejected the reflected property proposal.");
	EXPECT_TRUE(Object->Changes.empty());
	EXPECT_FALSE(Package->IsDirty());
	Object->PreChange = [Property](FPropertyEditProposal& Proposal) -> std::expected<void, FObjectValidationError> {
		*Property->ContainerPtrToValuePtr<int32>(Proposal.DraftLeafContainer, Proposal.DraftLeafArrayIndex) = 7;
		return {};
	};
	ASSERT_EQ(Handle->SetValue(19), FPropertyAccess::Success);
	EXPECT_FALSE(Package->IsDirty());
	Object->PreChange = {};
	std::optional<int32> ValueAtFirstDirty;
	const auto Listener = OnAssetPackagesChanged().AddLambda([&] {
		if (Package->IsDirty() && !ValueAtFirstDirty) ValueAtFirstDirty = Object->Value;
	});
	const auto Result = Handle->SetValue(19, EPropertyValueSetFlags::InteractiveChange);
	OnAssetPackagesChanged().Remove(Listener);
	ASSERT_EQ(Result, FPropertyAccess::Success);
	EXPECT_EQ(ValueAtFirstDirty, 7);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(Object->Value, 19);
}

TEST(FReflectedPropertyEditSessionTests, StandaloneWriteReleasesReplacedReference)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	InitializeDObjectSystem();
	using FNode = DMaterialExpressionMakeMaterialAttributes;
	TStrongObjectPtr<FNode> Owner(NewObject<FNode>(nullptr, "PropertyWriteOwner"));
	TWeakObjectPtr<FNode> Previous(NewObject<FNode>(nullptr, "PreviousInput"));
	Owner->BaseColor.Expression = Previous.Get();
	const auto Handle = MakePropertyHandle(*Owner, "BaseColor");
	ASSERT_EQ(Handle->SetValue(FMaterialExpressionInput{}, EPropertyValueSetFlags::NotTransactable), FPropertyAccess::Success);
	CollectGarbage(EObjectFlags::Standalone);
	EXPECT_FALSE(Previous.IsValid());
}

TEST(FReflectedPropertyEditSessionTests, CancellingRecordingDuringModifyPublishesWithoutUndo)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	auto* Package = MakeReflectedRevisionTestPackage();
	auto* Object = NewObject<DReflectedTransactionTestObject>(Package, "RetiredRecordingTarget");
	Object->Value = 7;
	FTestTransactorOwner Transactions;
	const auto Handle = MakePropertyHandle(*Object, "Value");
	Package->ClearDirty();
	const auto Listener = OnAssetPackagesChanged().AddLambda([&] {
		if (Package->IsDirty() && Transactions.Get()->IsActive()) Transactions.Get()->Cancel();
	});
	const auto Result = Handle->SetValue(19);
	OnAssetPackagesChanged().Remove(Listener);
	EXPECT_EQ(Result, FPropertyAccess::Success);
	ASSERT_EQ(Object->Changes.size(), 1u);
	EXPECT_EQ(Object->Changes.front().ChangeType, EPropertyChangeType::ValueSet);
	EXPECT_EQ(Object->Value, 19);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(Transactions.Get()->CanUndo());
}

TEST(FReflectedPropertyEditSessionTests, RetiringTargetInPreEditRejectsWrite)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	auto* Package = MakeReflectedRevisionTestPackage();
	auto* Object = NewObject<DReflectedTransactionTestObject>(Package, "RetiredProposalTarget");
	Object->Value = 7;
	const auto Handle = MakePropertyHandle(*Object, "Value");
	Package->ClearDirty();
	Object->PreChange = [&](FPropertyEditProposal&) -> std::expected<void, FObjectValidationError> {
		MarkAsGarbage(Object);
		return {};
	};
	EXPECT_EQ(Handle->SetValue(19, EPropertyValueSetFlags::NotTransactable), FPropertyAccess::Fail);
	EXPECT_EQ(Object->PreChangeCount, 1u);
	EXPECT_TRUE(Object->Changes.empty());
	EXPECT_EQ(Object->Value, 7);
	EXPECT_FALSE(Package->IsDirty());
}

TEST(FReflectedPropertyEditSessionTests, RetiringTargetDuringDirtyNotificationRejectsPublication)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	auto* Package = MakeReflectedRevisionTestPackage();
	auto* Object = NewObject<DReflectedTransactionTestObject>(Package, "RetiredPropertyTarget");
	Object->Value = 7;
	const auto Handle = MakePropertyHandle(*Object, "Value");
	Package->ClearDirty();
	const auto Listener = OnAssetPackagesChanged().AddLambda([&] {
		if (Package->IsDirty() && IsValid(Object)) MarkAsGarbage(Object);
	});
	const auto Result = Handle->SetValue(19, EPropertyValueSetFlags::NotTransactable);
	OnAssetPackagesChanged().Remove(Listener);
	EXPECT_FALSE(IsValid(Object));
	EXPECT_EQ(Result, FPropertyAccess::Fail);
	EXPECT_EQ(Handle->GetLastError(), "The property edit target is unavailable.");
	EXPECT_EQ(Object->Value, 7);
	EXPECT_TRUE(Object->Changes.empty());
}

TEST(FReflectedPropertyEditSessionTests, ShutdownRequestDuringProposalAllowsPendingWriteAndRejectsNextEdit)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	FTestTransactorOwner Editor;
	auto* Object = NewObject<DReflectedTransactionTestObject>(nullptr, "ShutdownPropertyTarget");
	Object->PreChange = [](FPropertyEditProposal&) -> std::expected<void, FObjectValidationError> {
		GEditor->RequestShutdown();
		return {};
	};
	const auto Handle = MakePropertyHandle(*Object, "Value");
	EXPECT_EQ(Handle->SetValue(19, EPropertyValueSetFlags::NotTransactable), FPropertyAccess::Success);
	EXPECT_EQ(Object->LastProposalFlags, EPropertyValueSetFlags::NotTransactable);
	EXPECT_EQ(Handle->SetValue(23, EPropertyValueSetFlags::NotTransactable), FPropertyAccess::Fail);
	EXPECT_EQ(Object->Value, 19);
	ASSERT_EQ(Object->Changes.size(), 1u);
	EXPECT_EQ(Object->Changes.front().ChangeType, EPropertyChangeType::ValueSet);
}

TEST(FReflectedPropertyEditSessionTests, EndingGestureDuringModifyFinishesAfterPublication)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	auto* Package = MakeReflectedRevisionTestPackage();
	auto* Object = NewObject<DReflectedTransactionTestObject>(Package, "UnrecordedPreview");
	Object->Value = 7;
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	FPropertyControl Edit;
	ASSERT_TRUE(Edit.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(FPropertyEditTarget::ForMember(Object, Property)), false));
	Package->ClearDirty();
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Edit, int32{11}));
	EXPECT_EQ(Object->Value, 11);
	Package->ClearDirty();
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_EQ(Object->Changes.size(), 1u);
	EXPECT_EQ(Object->Changes.front().ChangeType, EPropertyChangeType::Interactive);

	const auto Listener = OnAssetPackagesChanged().AddLambda([&] {
		if (Package->IsDirty() && Edit.IsActive()) EXPECT_TRUE(Edit.Finish(false));
	});
	const bool bWritten = Durin::Testing::ApplyControlInput(Edit, int32{19});
	OnAssetPackagesChanged().Remove(Listener);
	EXPECT_TRUE(bWritten);
	EXPECT_FALSE(Edit.IsActive());
	EXPECT_EQ(Object->Value, 19);
	EXPECT_EQ(Edit.GetHandle(), nullptr);
	ASSERT_EQ(Object->Changes.size(), 3u);
	EXPECT_EQ(Object->Changes[1].ChangeType, EPropertyChangeType::Interactive);
	EXPECT_EQ(Object->Changes.back().ChangeType, EPropertyChangeType::ValueSet);
}

TEST(FReflectedPropertyEditSessionTests, RecordedEditEnrollsOrdinaryTargetAndRestoresInitialPackageDirtyState)
{
	for (const bool bInitiallyDirty : {false, true})
	{
		SCOPED_TRACE(bInitiallyDirty ? "initially dirty" : "initially clean");
		auto* Package = MakeReflectedRevisionTestPackage();
		auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(Package, "FocusedFallbackTarget");
		Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(Object);
		Object->ClearFlags(Durin::EObjectFlags::Transactional);
		auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
		Object->Value = 19;
		Durin::FPropertyValueSnapshot Proposed;
		ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, Proposed));
		Object->Value = 7;
		Package->ClearDirty();
		if (bInitiallyDirty) Package->MarkDirty();
		FTestTransactorOwner Transactions;
		Durin::PropertyEditor::FPropertyControl Session;
		ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Durin::FPropertyEditTarget::ForMember(Object, Property)), true));
		EXPECT_EQ(Package->IsDirty(), bInitiallyDirty);
		ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, Proposed));
		EXPECT_TRUE(Object->HasAnyObjectFlags(Durin::EObjectFlags::Transactional));
		ASSERT_TRUE(Session.Finish(false));
		EXPECT_TRUE(Package->IsDirty());
		ASSERT_TRUE(Transactions.Get()->Undo());
		EXPECT_EQ(Object->Value, 7);
		EXPECT_EQ(Package->IsDirty(), bInitiallyDirty);
		ASSERT_TRUE(Transactions.Get()->Redo());
		EXPECT_EQ(Object->Value, 19);
		EXPECT_TRUE(Package->IsDirty());
	}
}

TEST(FReflectedPropertyEditSessionTests, HostShutdownKeepsDragValueAndCannotEndReplacementRecording)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DReflectedTransactionTestObject> Object(NewObject<DReflectedTransactionTestObject>(nullptr, "HostPreview"));
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 7;
	FPropertyValueSnapshot Preview;
	ASSERT_TRUE(CapturePropertyValue(Property, Object.Get(), 0, Preview));
	Object->Value = 3;
	Durin::PropertyEditor::FPropertyControl Session;
	{
		Tests::FTestEditorTransactorOwner First;
		ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(FPropertyEditTarget::ForMember(Object.Get(), Property)), true));
		ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, Preview));
		ASSERT_EQ(Object->Value, 7);
	}
	EXPECT_EQ(Object->Value, 7);
	Tests::FTestEditorTransactorOwner Second;
	FScopedTransaction Next("Replacement");
	ASSERT_TRUE(Next.IsOutstanding());
	EXPECT_FALSE(Session.Finish(true));
	EXPECT_TRUE(Second->IsActive());
}

TEST(FReflectedPropertyEditSessionTests, NoOpCreatesNoHistoryAndInterruptedDragIsUndoable)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "NoOpTransactionTarget");
	Durin::TStrongObjectPtr<Durin::DObject> ObjectRoot(Object);
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 3;
	const auto Target = Durin::FPropertyEditTarget::ForMember(Object, Property);
	Object->Value = 9;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, Proposed));
	Object->Value = 3;
	FTestTransactorOwner Transactions;

	Durin::PropertyEditor::FPropertyControl NoOpSession;
	ASSERT_TRUE(NoOpSession.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true));
	EXPECT_TRUE(NoOpSession.Finish(false));

	Durin::PropertyEditor::FPropertyControl CancelledSession;
	ASSERT_TRUE(CancelledSession.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(CancelledSession, Proposed));
	EXPECT_TRUE(CancelledSession.Finish(true));
	EXPECT_EQ(Object->Value, 9);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 3);
	EXPECT_FALSE(Transactions.Get()->CanUndo());
}

TEST(FReflectedPropertyEditSessionTests, TransactionHistoryKeepsTargetAlive)
{
	InitializeDObjectSystem();
	auto* Object = Durin::NewObject<DReflectedTransactionTestObject>(nullptr, "TransactionalPropertyTarget");
	auto* Property = DReflectedTransactionTestObject::FindProperty("Value");
	Object->Value = 4;
	const auto Target = Durin::FPropertyEditTarget::ForMember(Object, Property);
	Object->Value = 12;
	Durin::FPropertyValueSnapshot Proposed;
	ASSERT_TRUE(Durin::CapturePropertyValue(Property, Object, 0, Proposed));
	Object->Value = 4;
	FTestTransactorOwner Transactions;
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, Proposed));
	ASSERT_TRUE(Session.Finish(false));

	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(Durin::GDObjectArray.Contains(Object));
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 4);
	Transactions.Get()->Reset();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
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
	const auto Target = Durin::FPropertyEditTarget::ForMember(Owner, Property);
	FTestTransactorOwner Transactions;
	Durin::PropertyEditor::FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, Proposed));
	ASSERT_TRUE(Session.Finish(false));

	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
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
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Owner));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(Before));
	EXPECT_FALSE(Durin::GDObjectArray.Contains(After));
}

TEST(FReflectedPropertyEditSessionTests, RejectedRecordingKeepsOnlyTransactionStartedEdits)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	InitializeDObjectSystem();
	FTestTransactorOwner Transactions;
	TStrongObjectPtr<DReflectedTransactionTestObject> Object(NewObject<DReflectedTransactionTestObject>(nullptr, "RejectedProperty"));
	TStrongObjectPtr<DReflectedTransactionTestObject> Other(NewObject<DReflectedTransactionTestObject>(nullptr, "CallbackEdit"));
	const auto Handle = MakePropertyHandle(*Object, "Value");
	const auto Target = FPropertyEditTarget::ForMember(Object.Get(), Object->GetClass()->FindPropertyByName("Value"));
	FPropertyControl Session;
	for (bool bBeginSession : {false, true})
	{
		for (bool bEditOther : {false, true})
		{
			SCOPED_TRACE(bBeginSession);
			SCOPED_TRACE(bEditOther);
			const auto Listener = Transactions.Get()->OnTransactionStateChanged().AddLambda(
				[&](const FTransactionContext&, ETransactionStateEventType Event) {
					if (Event != ETransactionStateEventType::TransactionStarted) return;
					Object->SetFlags(EObjectFlags::DefaultSubobject);
					if (bEditOther)
					{
						Other->Modify(false);
						Other->Value = 19;
					}
				});
			if (bBeginSession)
			{
				ASSERT_TRUE(Session.BeginGesture(MakePropertyHandle(Target), true));
				EXPECT_FALSE(Durin::Testing::ApplyControlInput(Session, int32{7}));
				EXPECT_TRUE(Session.Finish(false));
			}
			else EXPECT_EQ(Handle->SetValue(7), FPropertyAccess::Fail);
			Transactions.Get()->OnTransactionStateChanged().Remove(Listener);
			Object->ClearFlags(EObjectFlags::DefaultSubobject);
			EXPECT_FALSE(Session.IsActive());
			EXPECT_FALSE(Transactions.Get()->IsActive());
			EXPECT_EQ(Object->Value, 0);
			ASSERT_EQ(Transactions.Get()->GetQueueLength(), bEditOther ? 1u : 0u);
			if (bEditOther)
			{
				EXPECT_EQ(Other->Value, 19);
				ASSERT_TRUE(Transactions.Get()->Undo());
				EXPECT_EQ(Other->Value, 0);
				Transactions.Get()->Reset();
			}
		}
	}
}

TEST(FReflectedPropertyEditSessionTests, FailedPreviewRestoreKeepsReflectedMemberUndoable)
{
	using namespace Durin;
	using namespace Durin::PropertyEditor;
	InitializeDObjectSystem();
	FTestTransactorOwner Transactions;
	TStrongObjectPtr<DReflectedTransactionTestObject> Object(NewObject<DReflectedTransactionTestObject>(nullptr, "InterruptedPreview"));
	const auto Target = FPropertyEditTarget::ForMember(Object.Get(), Object->FindProperty("Value"));
	Object->Value = 5;
	FPropertyControl Session;
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true, true));
	ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, int32{8}));

	// Restoring this member from inside its own edit hook is rejected as a nested write.
	std::string Error;
	Object->PreChange = [&](FPropertyEditProposal&) -> std::expected<void, FObjectValidationError> {
		EXPECT_FALSE(Session.Finish(true, &Error));
		return std::unexpected(FObjectValidationError{.Code = EObjectValidationError::PropertyRejected,
			.PropertyReason = EPropertyEditRejection::Rejected});
	};
	const auto Handle = MakePropertyHandle(Target);
	EXPECT_EQ(Handle->SetValue(9), FPropertyAccess::Fail);
	EXPECT_EQ(Error, "A reflected property hook cannot start a nested edit of the same target.");
	EXPECT_FALSE(Session.IsActive());
	EXPECT_FALSE(Transactions.Get()->IsActive());
	ASSERT_EQ(Object->Value, 8);
	ASSERT_EQ(Transactions.Get()->GetQueueLength(), 1u);
	Object->PreChange = {};
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 5);
	ASSERT_TRUE(Transactions.Get()->Redo());
	EXPECT_EQ(Object->Value, 8);
}

TEST(FReflectedPropertyEditSessionTests, UnchangedNestedSessionPreservesOuterRecording)
{
	using namespace Durin; using namespace Durin;
	InitializeDObjectSystem(); FTestTransactorOwner Transactions;
	TStrongObjectPtr<DReflectedTransactionTestObject> First(NewObject<DReflectedTransactionTestObject>(nullptr, "OuterSessionTarget"));
	TStrongObjectPtr<DReflectedTransactionTestObject> Second(NewObject<DReflectedTransactionTestObject>(nullptr, "UnchangedSessionTarget"));
	{
		Durin::FScopedTransaction Outer("test", "Outer edit", nullptr);
		ASSERT_TRUE(First->Modify()); First->Value = 7;
		Durin::PropertyEditor::FPropertyControl Session;
		ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(FPropertyEditTarget::ForMember(Second.Get(), Second->GetClass()->FindPropertyByName("Value"))), true));
		EXPECT_TRUE(Session.Finish(false));
		EXPECT_TRUE(Transactions.Get()->IsActive());
	}
	ASSERT_TRUE(Transactions.Get()->Undo()); EXPECT_EQ(First->Value, 0);
}

TEST(FReflectedPropertyEditSessionTests, CancellingChangedNestedPreviewPreservesOuterUndo)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FTestTransactorOwner Transactions;
	TStrongObjectPtr<DReflectedTransactionTestObject> Object(NewObject<DReflectedTransactionTestObject>(nullptr, "NestedPreview"));
	{
		FScopedTransaction Outer("Outer edit");
		ASSERT_TRUE(Object->Modify());
		Object->Value = 5;
		PropertyEditor::FPropertyControl Session;
		ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(FPropertyEditTarget::ForMember(Object.Get(), Object->FindProperty("Value"))), true, true));
		ASSERT_TRUE(Durin::Testing::ApplyControlInput(Session, int32{8}));
		ASSERT_TRUE(Session.Finish(true));
		EXPECT_FALSE(Session.IsActive());
		EXPECT_EQ(Object->Value, 5);
		EXPECT_TRUE(Transactions.Get()->IsActive());
	}
	ASSERT_EQ(Transactions.Get()->GetQueueLength(), 1u);
	ASSERT_TRUE(Transactions.Get()->Undo());
	EXPECT_EQ(Object->Value, 0);
}

TEST(FReflectedPropertyEditSessionTests, DisablingThePropertyScopePreservesOuterRecording)
{
	InitializeDObjectSystem();
	using namespace Durin;
	TStrongObjectPtr<DReflectedTransactionTestObject> Object(NewObject<DReflectedTransactionTestObject>(nullptr, "AdmissionProperty"));
	const auto Target = FPropertyEditTarget::ForMember(Object.Get(), Object->GetClass()->FindPropertyByName("Value"));
	Durin::PropertyEditor::FPropertyControl Session;
	EXPECT_FALSE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), true));
	EXPECT_FALSE(Session.IsActive());
	ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), false));
	ASSERT_TRUE(Session.Finish(true));
	Tests::FTestEditorTransactorOwner History;
	{
		FScopedTransaction Outer("Outer property edit");
		ASSERT_TRUE(Object->Modify());
		Object->Value = 7;
		ASSERT_TRUE(Session.BeginGesture(Durin::PropertyEditor::MakePropertyHandle(Target), false));
		ASSERT_TRUE(Session.Finish(false));
		EXPECT_TRUE(History->IsActive());
		EXPECT_EQ(Object->Value, 7);
	}
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(Object->Value, 0);
	EXPECT_FALSE(History->CanUndo());
	ASSERT_TRUE(History->Redo());
	EXPECT_EQ(Object->Value, 7);
}
