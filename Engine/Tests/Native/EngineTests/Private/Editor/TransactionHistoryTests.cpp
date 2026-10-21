#include "Editor/TransactionRecordTestSupport.h"

using namespace Durin::Tests::ObjectState;

TEST(FTransBufferTests, ExposesReflectedIdentityAndProductionMemoryBudget)
{
	InitializeDObjectSystem();
	EXPECT_TRUE(Durin::DTransactor::StaticClass()->HasAnyClassFlags(
		Durin::EClassFlags::Abstract));
	EXPECT_FALSE(Durin::DTransBuffer::StaticClass()->HasAnyClassFlags(
		Durin::EClassFlags::Abstract));
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "IdentityTransBuffer");
	ASSERT_NE(Buffer, nullptr);
	EXPECT_TRUE(Buffer->IsA<Durin::DTransactor>());
	EXPECT_EQ(Buffer->GetMemoryBudget(), 64u * 1024u * 1024u);
}

TEST(FTransBufferTests, CompositeParticipantsReceiveHooksWithoutEditingGcDependencies)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(nullptr, "CompositeFirst"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(nullptr, "CompositeSecond"));
	TStrongObjectPtr<DTransactionRecordParticipant> Dependency(NewObject<DTransactionRecordParticipant>(nullptr, "CompositeDependency"));
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::make_unique<FDeclaredParticipantChange>(First.Get(), Second.Get(), Dependency.Get())));
	const auto* Transaction = Buffer->GetTransaction(0);
	ASSERT_NE(Transaction, nullptr);
	EXPECT_TRUE(Transaction->ContainsObject(First.Get()));
	EXPECT_TRUE(Transaction->ContainsObject(Second.Get()));
	EXPECT_FALSE(Transaction->ContainsObject(Dependency.Get()));
	EXPECT_EQ(Transaction->GetTransactionObjects().size(), 2u);
	const auto FirstKey = FObjectKey(First.Get());
	First = TStrongObjectPtr<DTransactionRecordParticipant>(nullptr);
	CollectGarbage();
	auto* FirstObject = static_cast<DTransactionRecordParticipant*>(ResolveObjectKey(FirstKey));
	ASSERT_NE(FirstObject, nullptr);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(FirstObject->Value, 0);
	EXPECT_EQ(Second->Value, 0);
	EXPECT_EQ(FirstObject->UndoBegins, 1);
	EXPECT_EQ(FirstObject->UndoCompletions, 1);
	EXPECT_EQ(Second->UndoBegins, 1);
	EXPECT_EQ(Second->UndoCompletions, 1);
	EXPECT_EQ(Dependency->UndoBegins, 0);
	EXPECT_EQ(Dependency->UndoCompletions, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(FirstObject->Value, 3);
	EXPECT_EQ(Second->Value, 5);
	EXPECT_EQ(FirstObject->UndoCompletions, 2);
	ASSERT_TRUE(Buffer->Reset());
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(FirstKey), nullptr);
}

TEST(FTransBufferTests, CompositeParticipantPackagesAndExpiryAreExplicit)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> FirstPackage(NewObject<DPackage>(nullptr, "ParticipantPackageFirst"));
	TStrongObjectPtr<DPackage> SecondPackage(NewObject<DPackage>(nullptr, "ParticipantPackageSecond"));
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(FirstPackage.Get(), "First"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(SecondPackage.Get(), "Second"));
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::make_unique<FDeclaredParticipantChange>(First.Get(), Second.Get(), nullptr)));
	const auto Packages = Buffer->GetTransaction(0)->GetAffectedPackages();
	EXPECT_EQ(Packages.size(), 2u);
	EXPECT_TRUE(std::ranges::contains(Packages, FirstPackage.Get()));
	EXPECT_TRUE(std::ranges::contains(Packages, SecondPackage.Get()));
	MarkAsGarbage(Second.Get());
	EXPECT_FALSE(Buffer->Undo());
	EXPECT_EQ(First->Value, 3);
	EXPECT_EQ(First->UndoBegins, 0);
	EXPECT_EQ(First->UndoCompletions, 0);
}

TEST(FTransBufferTests, RuntimeBoundaryPreservesOperationContextAndRejectsDeferredFeatures)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "RuntimeBoundChange"));
	ASSERT_EQ(Buffer->Begin("runtime", "Runtime bound command"), 0);
	ITransaction& Recording = *GUndo;
	Recording.SetPrimaryObject(Target.Get());
	const auto Context = Recording.GetContext();
	EXPECT_NE(Context.TransactionId, 0u);
	EXPECT_NE(Context.OperationId, 0u);
	EXPECT_EQ(Context.PrimaryObject, Target.Get());
	EXPECT_FALSE(Recording.BeginOperation());
	ASSERT_EQ(Buffer->Begin("nested", "Nested"), 1);
	EXPECT_EQ(GUndo->GetContext().OperationId, Context.OperationId);
	EXPECT_EQ(Buffer->End(), 2);
	EXPECT_FALSE(Recording.SaveArray({.Object = Target.Get()}));
	EXPECT_FALSE(Recording.ContainsPieObjects());
	std::vector<uint64> ReplayOperations;
	auto Change = std::make_unique<FTestCustomChange>(Target->Value, 0, 9);
	Change->ReplayProbe = [&] { ReplayOperations.push_back(Buffer->GetTransaction(0)->GetContext().OperationId); };
	Target->Value = 9;
	ASSERT_TRUE(Recording.StoreUndo(Target.Get(), std::move(Change)));
	EXPECT_EQ(Buffer->End(), 1);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetContext().OperationId, 0u);
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_TRUE(Buffer->Redo());
	ASSERT_EQ(ReplayOperations.size(), 2u);
	EXPECT_NE(ReplayOperations[0], 0u);
	EXPECT_NE(ReplayOperations[0], ReplayOperations[1]);
	EXPECT_NE(ReplayOperations[0], Context.OperationId);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetContext().OperationId, 0u);
}

TEST(FTransBufferTests, ModifyUsesVirtualSerializationAndOneRecordPerObject)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "VirtualModify"));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Native and reflected data"});
		ASSERT_TRUE(Target->Modify()); ASSERT_TRUE(Target->Modify());
		EXPECT_EQ(Target->SerializationSaves, 1);
		Target->Value = 3; Target->NativeValue = 7;
	}
	EXPECT_EQ(Target->SerializationSaves, 2);
	ASSERT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->NativeValue, 0);
	EXPECT_EQ(Target->SerializationLoads, 1);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 3); EXPECT_EQ(Target->NativeValue, 7);
	EXPECT_EQ(Target->SerializationLoads, 2);
}

TEST(FTransBufferTests, ExplicitRecordsAndModifyShareFirstBeforeAndFinalAfter)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "RepeatedModify"));
	Target->NativeValue = 5;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Repeated whole capture"});
		auto Record = FTransactionObjectRecord::CaptureWhole(*Target);
		ASSERT_TRUE(Record); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Record)));
		Target->Value = 1;
		ASSERT_TRUE(Target->Modify()); Target->Value = 4; Target->SessionValue = 9; Target->NativeValue = 10;
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SessionValue, 0); EXPECT_EQ(Target->NativeValue, 5);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 4); EXPECT_EQ(Target->SessionValue, 9); EXPECT_EQ(Target->NativeValue, 10);
}

TEST(FTransBufferTests, SerializedMetadataRestoresNameOuterAndFlagsWithoutChangingIdentity)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DObject> First(NewObject<DObject>(nullptr, "MetadataFirstOuter"));
	TStrongObjectPtr<DObject> Second(NewObject<DObject>(nullptr, "MetadataSecondOuter"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(First.Get(), "BeforeMetadata"));
	const auto Key = FObjectKey(Target.Get());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Metadata"}); ASSERT_TRUE(Target->Modify());
		Target->Rename("AfterMetadata"); Target->SetOuterPrivate(Second.Get()); Target->SetFlags(EObjectFlags::Public);
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->GetFName(), FName("BeforeMetadata")); EXPECT_EQ(Target->GetOuter(), First.Get());
	EXPECT_FALSE(Target->HasAnyObjectFlags(EObjectFlags::Public)); EXPECT_EQ(FObjectKey(Target.Get()), Key);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->GetFName(), FName("AfterMetadata")); EXPECT_EQ(Target->GetOuter(), Second.Get());
	EXPECT_TRUE(Target->HasAnyObjectFlags(EObjectFlags::Public)); EXPECT_EQ(FObjectKey(Target.Get()), Key);
}

TEST(FTransBufferTests, WholeObjectFlipRetainsLivePreUndoStateAndCompensatesLoadFailure)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "LiveFlipState"));
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Flip state"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; Target->NativeValue = 2; }
	Target->Value = 11; Target->NativeValue = 22; Target->bFailLoadOnce = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Target->Value, 11); EXPECT_EQ(Target->NativeValue, 22);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u); EXPECT_TRUE(Buffer->CanUndo());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->NativeValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 11); EXPECT_EQ(Target->NativeValue, 22);
}

TEST(FTransBufferTests, NativeExceptionAndCompensationFailureRequireRecovery)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem();
	for (bool bNativeException : {false, true})
	{
		SCOPED_TRACE(bNativeException);
		Tests::FTestTransactorOwner Buffer;
		TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "FailedNativeReplay"));
		{ FScopedTransaction Scope(Buffer.Get(), {"test", "Native replay failure"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
		Target->Value = 11; Target->NativeValue = 22;
		Target->bThrowLoadOnce = bNativeException;
		Target->FailedLoadsRemaining = bNativeException ? 0 : 2;
		EXPECT_FALSE(Buffer->Undo());
		EXPECT_EQ(Target->Value, 11); EXPECT_EQ(Target->NativeValue, 22);
		EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
		EXPECT_EQ(Buffer->GetUndoCount(), 0u);
		ASSERT_TRUE(Buffer->GetLastResult().ApplyCause);
		EXPECT_EQ(Buffer->GetLastResult().ApplyCause->Status, ETransactionApplyFailure::RecoveryRequired);
	}
}

TEST(FTransBufferTests, WholeObjectContainersPreserveExcludedFieldsOfMatchingElements)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "NestedContainerState"));
	Target->NestedArray = {{.Count = 1, .Session = 2, .Ignored = 3}};
	Target->NestedMap["same"] = {.Count = 4, .Session = 5, .Ignored = 6};
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Nested container projection"}); ASSERT_TRUE(Target->Modify());
		Target->NestedArray[0] = {.Count = 10, .Session = 20, .Ignored = 30};
		Target->NestedArray.push_back({.Count = 11});
		Target->NestedMap["same"] = {.Count = 40, .Session = 50, .Ignored = 60};
		Target->NestedMap["added"] = {.Count = 12};
	}
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_EQ(Target->NestedArray.size(), 1u);
	EXPECT_EQ(Target->NestedArray[0].Count, 1); EXPECT_EQ(Target->NestedArray[0].Session, 2); EXPECT_EQ(Target->NestedArray[0].Ignored, 30);
	ASSERT_EQ(Target->NestedMap.size(), 1u);
	EXPECT_EQ(Target->NestedMap.at("same").Count, 4); EXPECT_EQ(Target->NestedMap.at("same").Session, 5); EXPECT_EQ(Target->NestedMap.at("same").Ignored, 60);
	Target->NestedArray[0].Ignored = 31; Target->NestedMap.at("same").Ignored = 61;
	ASSERT_TRUE(Buffer->Redo());
	ASSERT_EQ(Target->NestedArray.size(), 2u); ASSERT_EQ(Target->NestedMap.size(), 2u);
	EXPECT_EQ(Target->NestedArray[0].Count, 10); EXPECT_EQ(Target->NestedArray[0].Ignored, 31);
	EXPECT_EQ(Target->NestedMap.at("same").Count, 40); EXPECT_EQ(Target->NestedMap.at("same").Ignored, 61);
}

TEST(FTransBufferTests, ThrowingWholeObjectNotificationRequiresRecoveryAndBalancesCompletion)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingObjectObserver"));
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Observer exception"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	Target->bThrowPropertyNotification = true;
	EXPECT_FALSE(Buffer->Undo());
	EXPECT_EQ(Buffer->GetLastResult().Code, ETransactorResultCode::RecoveryRequired);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u); EXPECT_EQ(Target->UndoBegins, 1); EXPECT_EQ(Target->UndoCompletions, 1);
	Target->bThrowPropertyNotification = false;
}

TEST(FTransBufferTests, MultipleWholeCapturesCoalesceAcrossNestedMemberEdits)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CanonicalWholeMembers"));
	auto RecordEdit = [&](int32& Value, int32 Next) -> bool {
		auto Record = FTransactionObjectRecord::CaptureWhole(*Target);
		if (!Record || !FTransBufferTestAccess::Record(*Buffer, std::move(*Record))) return false;
		Value = Next;
		return true;
	};
	{
		FScopedTransaction Outer(Buffer.Get(), {"test", "Multiple members"});
		ASSERT_TRUE(RecordEdit(Target->Value, 1));
		{ FScopedTransaction Inner(Buffer.Get(), {"test", "Nested member"}); ASSERT_TRUE(RecordEdit(Target->SessionValue, 2)); }
		ASSERT_TRUE(RecordEdit(Target->Value, 3));
	}
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SessionValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 3); EXPECT_EQ(Target->SessionValue, 2);
	{
		FScopedTransaction Outer(Buffer.Get(), {"test", "Modify before explicit capture"}); ASSERT_TRUE(Target->Modify());
		ASSERT_TRUE(RecordEdit(Target->Value, 4)); Target->NativeValue = 6;
	}
	EXPECT_EQ(Buffer->GetTransaction(1)->GetRecordCount(), 1u);
	const auto Loads = Target->SerializationLoads;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 3); EXPECT_EQ(Target->NativeValue, 0); EXPECT_EQ(Target->SerializationLoads, Loads + 1);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 4); EXPECT_EQ(Target->NativeValue, 6);
}

TEST(FTransBufferTests, NativeOnlyChangeRetainsHistoryAndUnchangedNativeStatePrunes)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "NativeOnlyChange"));
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Native state"}); ASSERT_TRUE(Target->Modify()); Target->NativeValue = 13; }
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->NativeValue, 0);
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Native no-op"}); ASSERT_TRUE(Target->Modify()); Target->NativeValue = 7; Target->NativeValue = 0; }
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_TRUE(Buffer->CanRedo());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->NativeValue, 13);
}

TEST(FTransBufferTests, ScopedModifyRecordsChangedMembersAndPrunesNoOps)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ModifyTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "ModifyTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);

	{
		Durin::Editor::FScopedTransaction NoChange(Buffer, {"test", "No change"});
		(void)Target->Modify();
		(void)Target->Modify();
		EXPECT_EQ(EndRecording(NoChange, *Buffer).Code, Durin::Editor::ETransactorResultCode::NoOp);
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);

	{
		Durin::Editor::FScopedTransaction Change(Buffer, {"test", "Change value"});
		(void)Target->Modify();
		Target->Value = 42;
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 42);

	{
		Durin::Editor::FScopedTransaction Canceled(Buffer, {"test", "Canceled value"});
		(void)Target->Modify();
		Target->Value = 81;
		EXPECT_EQ(CancelRecording(Canceled, *Buffer).Code, Durin::Editor::ETransactorResultCode::Discarded);
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Target->Value, 81);
}

TEST(FTransBufferTests, UndoCountTracksUndoneEntriesAcrossReplayAndBranchReplacement)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "UndoCountBuffer"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "UndoCountTarget"));
	auto ExpectCounts = [&](size_t Undoable, size_t Undone) {
		EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), Undoable);
		EXPECT_EQ(Buffer->GetUndoCount(), Undone);
		EXPECT_EQ(Buffer->GetQueueLength(), Undoable + Undone);
	};
	auto Commit = [&] {
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Change value"});
		EXPECT_TRUE(Target->Modify());
		++Target->Value;
	};
	ExpectCounts(0, 0);
	Commit(); Commit(); Commit();
	ExpectCounts(3, 0);
	ASSERT_TRUE(Buffer->Undo());
	ExpectCounts(2, 1);
	ASSERT_TRUE(Buffer->Undo());
	ExpectCounts(1, 2);
	ASSERT_TRUE(Buffer->Undo());
	ExpectCounts(0, 3);
	ASSERT_TRUE(Buffer->Redo());
	ExpectCounts(1, 2);
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Cancel recording"});
		Scope.Cancel();
	}
	ExpectCounts(1, 2);
	Commit();
	ExpectCounts(2, 0);
	EXPECT_FALSE(Buffer->CanRedo());
	ASSERT_TRUE(Buffer->Reset());
	ExpectCounts(0, 0);
}

TEST(FTransBufferTests, NestedModifyPreservesOutermostFinalState)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "NestedModifyBuffer"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "NestedModifyTarget"));
	FScopedTransaction Outer(Buffer.Get(), {"test", "Nested object modification"});
	(void)Target.Get()->Modify();
	Target->Value = 1;
	{
		FScopedTransaction Inner(Buffer.Get(), {"test", "Inner modification"});
		(void)Target.Get()->Modify();
		Target->Value = 2;
	}
	Target->Value = 3;
	ASSERT_TRUE(EndRecording(Outer, *Buffer));
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 3);
}

TEST(FTransBufferTests, MoveOnlyScopeClosesOnceAndCancelIsIdempotent)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "MoveScopeTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "MoveScopeTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	{
		Durin::Editor::FScopedTransaction First(Buffer, {"test", "Moved scope"});
		(void)Target->Modify();
		++Target->Value;
		Durin::Editor::FScopedTransaction Second(std::move(First));
		EXPECT_FALSE(First.IsOutstanding());
		EXPECT_TRUE(Second.IsOutstanding());
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	Durin::Editor::FScopedTransaction Canceled(Buffer, {"test", "Canceled"});
	EXPECT_EQ(CancelRecording(Canceled, *Buffer).Code, Durin::Editor::ETransactorResultCode::Discarded);
	EXPECT_FALSE(Canceled.IsOutstanding());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
}

TEST(FTransBufferTests, MaintainsCursorBranchIdsAndEvents)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "HistoryTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "HistoryTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	auto Commit = [&](std::string Description) {
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", std::move(Description)});
		(void)Target->Modify();
		++Target->Value;
		return EndRecording(Scope, *Buffer);
	};

	const auto First = Commit("First");
	const auto Second = Commit("Second");
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);
	EXPECT_LT(First.TransactionId, Second.TransactionId);
	EXPECT_TRUE(Buffer->Undo());
	EXPECT_EQ(Buffer->GetUndoId(), First.TransactionId);
	EXPECT_EQ(Buffer->GetRedoId(), Second.TransactionId);
	EXPECT_TRUE(Buffer->Redo());
	EXPECT_EQ(Buffer->GetUndoId(), Second.TransactionId);
	EXPECT_TRUE(Buffer->Undo());
	const auto Branch = Commit("Branch");
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_GT(Branch.TransactionId, Second.TransactionId);

	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_EQ(Buffer->GetUndoContext(false).Description, "Branch");
	const auto Events = ReadTransactionEvents(*Buffer);
	ASSERT_GE(Events.size(), 7u);
	EXPECT_EQ(Events.back().Type, Durin::Editor::ETransactionEventType::Executed);

	ASSERT_TRUE(Buffer->Reset());
	const auto AfterReset = Commit("After reset");
	EXPECT_GT(AfterReset.TransactionId, Branch.TransactionId);
}

TEST(FTransBufferTests, OversizedRecordingRemainsUndoableAndPreservesFailedUndoCursor)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "FailureTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "FailureTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	Durin::FTransBufferTestAccess::InitializeMemoryBudget(*Buffer, 1);
	Durin::Editor::FScopedTransaction Oversized(Buffer, {"test", "Oversized"});
	(void)Target->Modify();
	Target->Numbers.push_back(1);
	ASSERT_TRUE(EndRecording(Oversized, *Buffer));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_GT(Buffer->GetOwnedBytes(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_TRUE(Target->Numbers.empty());
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Numbers, std::vector<int32>{1});

	Durin::Editor::FScopedTransaction Valid(Buffer, {"test", "Stale"});
	(void)Target->Modify();
	++Target->Value;
	ASSERT_TRUE(EndRecording(Valid, *Buffer));
	const auto Id = Buffer->GetUndoId();
	TargetRoot = Durin::TStrongObjectPtr<Durin::DObject>(nullptr);
	Durin::MarkAsGarbage(Target);
	Durin::CollectGarbage();
	EXPECT_EQ(Buffer->UndoExpected(Durin::Editor::FTransactionId{0}).Code, Durin::Editor::ETransactorResultCode::Failed);
	EXPECT_EQ(Buffer->GetUndoId(), Id);
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 1u);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
}

TEST(FTransBufferTests, ExpectedIdsPreserveHistoryPosition)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "AddressedTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "AddressedTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	auto Commit = [&](std::string Description) {
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", std::move(Description)});
		(void)Target->Modify();
		++Target->Value;
		return EndRecording(Scope, *Buffer);
	};
	const auto First = Commit("First");
	const auto Second = Commit("Second");
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);

	EXPECT_EQ(Buffer->UndoExpected(First.TransactionId).Code,
		Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->GetUndoId(), Second.TransactionId);
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 2u);
	ASSERT_TRUE(Buffer->UndoExpected(Second.TransactionId));
	EXPECT_EQ(Buffer->GetRedoId(), Second.TransactionId);
	EXPECT_EQ(Buffer->RedoExpected(First.TransactionId).Code,
		Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->GetRedoId(), Second.TransactionId);

	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 1u);
	ASSERT_TRUE(Buffer->RedoExpected(Second.TransactionId));
	EXPECT_EQ(Buffer->GetUndoId(), Second.TransactionId);
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 2u);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
}

TEST(FTransBufferTests, ForgetPackageRetiresHardSnapshotDependenciesOnBothSidesOfCursor)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "PackageDependencyBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "RetiredPackage");
	auto* Participant = Durin::NewObject<Durin::DObject>(Package, "RetiredParticipant");
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "ExternalTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	Durin::TWeakObjectPtr<Durin::DObject> WeakParticipant(Participant);
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Unrelated value"});
		(void)Target->Modify();
		++Target->Value;
		ASSERT_TRUE(EndRecording(Scope, *Buffer));
	}
	const auto UnrelatedId = Buffer->GetUndoId();
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Hard after dependency"});
		(void)Target->Modify();
		Target->Hard = Participant;
		ASSERT_TRUE(EndRecording(Scope, *Buffer));
	}
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Hard before dependency"});
		(void)Target->Modify();
		Target->Hard = nullptr;
		ASSERT_TRUE(EndRecording(Scope, *Buffer));
	}
	ASSERT_TRUE(Buffer->Undo());
	Buffer->ForgetPackage(*Package);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoId(), UnrelatedId);
	EXPECT_FALSE(Buffer->CanRedo());
	Target->Hard = nullptr;
	Durin::CollectGarbage();
	EXPECT_FALSE(WeakParticipant.IsValid());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
}

TEST(FTransBufferTests, ForgetPackageRetiresCustomLifetimeDependencies)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "CustomPackageDependencyBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "CustomRetiredPackage");
	auto* Participant = Durin::NewObject<Durin::DObject>(Package, "CustomRetiredParticipant");
	Durin::TWeakObjectPtr<Durin::DObject> WeakParticipant(Participant);
	int Value = 0;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1, Participant)));
	Buffer->ForgetPackage(*Package);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_EQ(Buffer->GetOwnedBytes(), 0u);
	EXPECT_EQ(Value, 1);
	Durin::CollectGarbage();
	EXPECT_FALSE(WeakParticipant.IsValid());
}

TEST(FTransBufferTests, CancellationEvictionAndDestructionReleaseCollectorEdges)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ReleaseTransBuffer");
	Durin::FTransBufferTestAccess::InitializeMemoryBudget(*Buffer, 1);
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto MakeTarget = [](std::string_view Name) {
		return Durin::NewObject<DTransactionRecordParticipant>(nullptr, Durin::FName(Name));
	};

	auto* Canceled = MakeTarget("CanceledTarget");
	const auto CanceledHandle = Durin::FObjectKey(Canceled);
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Canceled"});
		(void)Canceled->Modify();
		EXPECT_EQ(CancelRecording(Scope, *Buffer).Code, Durin::Editor::ETransactorResultCode::Discarded);
	}
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(CanceledHandle), nullptr);

	auto Commit = [&](DTransactionRecordParticipant* Target, std::string Description) {
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", std::move(Description)});
		(void)Target->Modify();
		++Target->Value;
		EXPECT_TRUE(EndRecording(Scope, *Buffer));
	};
	auto* Evicted = MakeTarget("EvictedTarget");
	const auto EvictedHandle = Durin::FObjectKey(Evicted);
	Commit(Evicted, "Evicted");
	auto* Retained = MakeTarget("RetainedTarget");
	Durin::TStrongObjectPtr<Durin::DObject> RetainedRoot(Retained);
	Commit(Retained, "Retained");
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(EvictedHandle), nullptr);

	const auto RetainedHandle = Durin::FObjectKey(Retained);
	RetainedRoot = Durin::TStrongObjectPtr<Durin::DObject>(nullptr);
	Buffer->BeginDestroy();
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(RetainedHandle), nullptr);
}

TEST(FTransBufferTests, CollectorRetainsPendingAndHistoryEdgesAndReleasesBranches)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "GCTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* First = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "GCFirstTarget");
	auto* Hard = Durin::NewObject<Durin::DObject>(nullptr, "GCHardValue");
	auto* Weak = Durin::NewObject<Durin::DObject>(nullptr, "GCWeakValue");
	First->Hard = Hard;
	First->Weak = Weak;
	const auto FirstHandle = Durin::FObjectKey(First);
	const auto HardHandle = Durin::FObjectKey(Hard);
	const auto WeakHandle = Durin::FObjectKey(Weak);
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "First GC"});
		(void)First->Modify();
		First->Hard = nullptr;
		First->Weak = nullptr;
		Durin::CollectGarbage();
		EXPECT_NE(Durin::ResolveObjectKey(FirstHandle), nullptr);
		EXPECT_NE(Durin::ResolveObjectKey(HardHandle), nullptr);
		EXPECT_EQ(Durin::ResolveObjectKey(WeakHandle), nullptr);
	}
	Durin::CollectGarbage();
	EXPECT_NE(Durin::ResolveObjectKey(FirstHandle), nullptr);
	EXPECT_NE(Durin::ResolveObjectKey(HardHandle), nullptr);
	ASSERT_TRUE(Buffer->Undo());

	auto* Second = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "GCSecondTarget");
	Durin::TStrongObjectPtr<Durin::DObject> SecondRoot(Second);
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Branch GC"});
		(void)Second->Modify();
		++Second->Value;
	}
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(FirstHandle), nullptr);
	EXPECT_EQ(Durin::ResolveObjectKey(HardHandle), nullptr);
	ASSERT_TRUE(Buffer->Reset());
	SecondRoot = Durin::TStrongObjectPtr<Durin::DObject>(nullptr);
	const auto SecondHandle = Durin::FObjectKey(Second);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(SecondHandle), nullptr);
}

TEST(FTransBufferTests, DetachedOwnedChildSurvivesCollectionUndoRedoAndReleasesWithHistory)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Buffer = NewObject<DTransBuffer>(nullptr, "DetachedChildHistory");
	TStrongObjectPtr<DObject> BufferRoot(Buffer);
	auto* Owner = NewObject<DTransactionRecordParticipant>(nullptr, "GraphOwner");
	TStrongObjectPtr<DObject> OwnerRoot(Owner);
	auto* Child = NewObject<DTransactionRecordParticipant>(Owner, "Expression");
	Child->Value = 71;
	Owner->Hard = Child;
	auto* HistoryOuter = NewObject<DObject>(nullptr, "DetachedGraphObjects");
	const auto ChildHandle = FObjectKey(Child);
	const auto HistoryHandle = FObjectKey(HistoryOuter);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FDetachedChildChange>(Owner, Child, HistoryOuter)));
	CollectGarbage();
	ASSERT_EQ(ResolveObjectKey(ChildHandle), Child);
	EXPECT_EQ(Child->GetOuter(), HistoryOuter);
	EXPECT_EQ(Owner->Hard.Get(), nullptr);
	EXPECT_TRUE(GDObjectArray.GetObjectsWithOuter(Owner, EObjectQueryScope::LiveOnly).empty());
	ASSERT_TRUE(Buffer->Undo());
	CollectGarbage();
	EXPECT_EQ(Owner->Hard.Get(), Child);
	EXPECT_EQ(Child->GetOuter(), Owner);
	EXPECT_EQ(Child->Value, 71);
	ASSERT_TRUE(Buffer->Redo());
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(ChildHandle), Child);
	EXPECT_EQ(Child->GetOuter(), HistoryOuter);
	ASSERT_TRUE(Buffer->Reset());
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(ChildHandle), nullptr);
	EXPECT_EQ(ResolveObjectKey(HistoryHandle), nullptr);
	EXPECT_EQ(Owner->Hard.Get(), nullptr);
}

TEST(FTransBufferTests, ExecutesCustomChangesAndPreservesCursorOnFailure)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "CustomChangeTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	int Value = 1;
	auto Change = std::make_unique<FTestCustomChange>(Value, 1, 2);
	auto* ChangePtr = Change.get();
	const auto Execute = Durin::Editor::ApplyEditorChange(*Buffer, std::move(Change));
	ASSERT_TRUE(Execute);
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetUndoId(), Execute.TransactionId);

	ChangePtr->bFailUndo = true;
	EXPECT_EQ(Buffer->UndoExpected(Durin::Editor::FTransactionId{0}).Code, Durin::Editor::ETransactorResultCode::Failed);
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetUndoId(), Execute.TransactionId);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	ChangePtr->bFailUndo = false;
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 2);
}

TEST(FTransBufferTests, SynchronousChangesJoinNestedRecordingAndPreserveOuterContext)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "GroupedCustomChanges"));
	int Value = 1;
	const auto Outer = BeginRecording(*Buffer, {"outer", "Grouped edit"});
	ASSERT_TRUE(Outer);
	const auto Inner = BeginRecording(*Buffer, {"inner", "Ignored description"});
	ASSERT_TRUE(Inner);
	auto First = std::make_unique<FTestCustomChange>(Value, 1, 2);

	const auto Joined = Durin::Editor::ApplyEditorChange(*Buffer, std::move(First));
	ASSERT_TRUE(Joined);
	EXPECT_EQ(Joined.TransactionId, Outer.TransactionId);
	EXPECT_EQ(Joined.TransactionId, Outer.TransactionId);
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	ASSERT_TRUE(EndRecording(*Buffer));
	auto Second = std::make_unique<FTestCustomChange>(Value, 2, 3);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::move(Second)));
	EXPECT_EQ(Buffer->UndoExpected(Durin::Editor::FTransactionId{0}).Code, ETransactorResultCode::Rejected);
	ASSERT_TRUE(EndRecording(*Buffer));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoContext(false).Description, "Grouped edit");
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 3);
}

TEST(FTransBufferTests, CancelingJoinedChangeDiscardsRecordingWithoutRevertingEdits)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "CanceledGroupedChanges"));
	int Value = 1;
	const auto Outer = BeginRecording(*Buffer);
	const auto Inner = BeginRecording(*Buffer);
	auto Change = std::make_unique<FTestCustomChange>(Value, 1, 2);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::move(Change)));
	EXPECT_EQ(CancelRecording(*Buffer, Inner.Index).Code, ETransactorResultCode::Discarded);
	EXPECT_EQ(Value, 2);
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
}

TEST(FTransBufferTests, JoinedAppliedChangeAndPropertyCaptureCommitTogetherAndReplaceRedoOnlyAtEnd)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "MixedGroupedChanges"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "GroupedProperty"));
	int Value = 1;
	const auto Prior = Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2));
	ASSERT_TRUE(Prior);
	ASSERT_TRUE(Buffer->Undo());
	FScopedTransaction Outer(Buffer.Get(), {.Description = "Property and applied change"});
	ASSERT_TRUE(Target->Modify());
	Target->Value = 42;
	Value = 3;
	auto Change = std::make_unique<FTestCustomChange>(Value, 1, 3);
	ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
	EXPECT_EQ(Buffer->GetRedoId(), Prior.TransactionId);
	ASSERT_TRUE(EndRecording(Outer, *Buffer));
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 3);
	EXPECT_EQ(Target->Value, 42);
}

TEST(FTransBufferTests, JoinedRecoveryRetainsOuterAndFailedParticipants)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "GroupedRecovery"));
	auto* FirstTarget = NewObject<DTransactionRecordParticipant>(nullptr, "GroupedFirstReference");
	auto* FailedTarget = NewObject<DTransactionRecordParticipant>(nullptr, "GroupedFailedReference");
	int Value = 1;
	const auto Outer = BeginRecording(*Buffer);
	auto First = std::make_unique<FTestCustomChange>(Value, 1, 2, FirstTarget);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::move(First)));
	auto Bad = std::make_unique<FTestCustomChange>(Value, 2, 3, FailedTarget);
	Bad->bThrowRedo = true;
	const auto Failure = Durin::Editor::ApplyEditorChange(*Buffer, std::move(Bad));
	EXPECT_EQ(Failure.Code, ETransactorResultCode::RecoveryRequired);
	ASSERT_NE(Failure.ApplyCause, nullptr);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_FALSE(Buffer->CanTransact());
	FCountingReferenceCollector Collector;
	Buffer->AddReferencedObjects(Collector);
	EXPECT_TRUE(std::ranges::contains(Collector.References, FirstTarget));
	EXPECT_TRUE(std::ranges::contains(Collector.References, FailedTarget));
}

TEST(FTransBufferTests, CustomReferencesAndModuleDrainReleaseHistory)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ModuleCustomTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Referenced = Durin::NewObject<Durin::DObject>(nullptr, "CustomReferencedObject");
	const Durin::FObjectKey Handle = Durin::FObjectKey(Referenced);
	int Value = 1;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(
		Value, 1, 2, Referenced, "TestModule")));
	Durin::CollectGarbage();
	EXPECT_NE(Durin::ResolveObjectKey(Handle), nullptr);
	ASSERT_TRUE(Buffer->DiscardCustomChangesByModule("TestModule"));
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(Handle), nullptr);
}

TEST(FTransBufferTests, FailedCompensationPreservesEveryMessageAndDisablesHistory)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	auto* Buffer = NewObject<DTransBuffer>(nullptr, "FailedCompensationTransBuffer");
	TStrongObjectPtr<DObject> Root(Buffer);
	auto* Package = NewObject<DPackage>(nullptr, "FailedCompensationPackage");
	TStrongObjectPtr<DObject> PackageRoot(Package);
	(void)MakeSoftPath();
	FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/FailedCompensation", PackagePath));
	Package->InitializeAssetPackage(PackagePath);
	Buffer->EstablishSavedState(*Package);
	int First = 1, Second = 2, Third = 3;
	FTransaction Transaction(1, {.Description = "Compensation test"});
	auto FailingUndo = std::make_unique<FTestCustomChange>(First, 0, 1);
	FailingUndo->bFailUndo = true;
	auto FailingRedo = std::make_unique<FTestCustomChange>(Second, 0, 2);
	FailingRedo->bFailRedo = true;
	auto OtherFailingRedo = std::make_unique<FTestCustomChange>(Third, 0, 3);
	OtherFailingRedo->bFailRedo = true;
	Transaction.AddRecord(std::move(FailingUndo));
	Transaction.AddRecord(std::move(FailingRedo));
	Transaction.AddRecord(std::move(OtherFailingRedo));
	Transaction.SetPackageTransitions({{.Package = FPersistentObjectRef(Package)}});
	FTransBufferTestAccess::InstallHistory(*Buffer, std::move(Transaction));
	const auto Result = Buffer->UndoExpected(Durin::Editor::FTransactionId{0});
	EXPECT_EQ(Result.Code, ETransactorResultCode::RecoveryRequired);
	ASSERT_TRUE(Result.ApplyCause);
	EXPECT_EQ(Result.ApplyCause->Status, ETransactionApplyFailure::RecoveryRequired);
	ASSERT_EQ(Result.ApplyCause->RollbackFailures.size(), 2u);
	EXPECT_EQ(Result.ApplyCause->RollbackFailures[0].RecordIndex, 1u);
	EXPECT_EQ(Result.ApplyCause->RollbackFailures[1].RecordIndex, 2u);
	EXPECT_EQ(Result.ApplyCause->RecordIndex, 0u);
	EXPECT_EQ(Result.ApplyCause->Message, "Injected mutation failure.");
	for (const auto& Failure : Result.ApplyCause->RollbackFailures)
		EXPECT_EQ(Failure.Message, "Injected mutation failure.");
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	ASSERT_TRUE(Buffer->GetPackageRevisionState(*Package));
	EXPECT_FALSE(Buffer->GetPackageRevisionState(*Package)->bCheckpointValid);
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 1u);
	EXPECT_FALSE(Buffer->CanUndo());
	EXPECT_FALSE(Buffer->CanRedo());
	EXPECT_EQ(Buffer->UndoExpected(Durin::Editor::FTransactionId{0}).Code, ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->RedoExpected(Durin::Editor::FTransactionId{0}).Code, ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->Reset().Code, ETransactorResultCode::Rejected);
	EXPECT_EQ((BeginRecording(*Buffer, {.Description = "Blocked"}), Buffer->GetLastResult()).Code, ETransactorResultCode::Rejected);
	EXPECT_EQ(First, 1);
	EXPECT_EQ(Second, 0);
	EXPECT_EQ(Third, 0);
}

TEST(FTransBufferTests, ValidationRejectsUnavailableCustomRecordBeforeExecution)
{
	using namespace Durin::Editor;
	int Value = 1;
	FTransaction Transaction(42, {.Description = "Typed validation"});
	Transaction.AddRecord(std::make_unique<FTestCustomChange>(Value, 1, 2));
	Transaction.AddRecord(std::unique_ptr<FActorCommandChange>{});
	const auto Rejected = Transaction.Apply(false, Durin::EPropertyChangeOrigin::Redo);
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Status, ETransactionApplyFailure::ValidationFailed);
	EXPECT_EQ(Rejected.error().RecordIndex, 1u);
	const auto RejectionMessage = Rejected.error().Message;
	EXPECT_EQ(Value, 1);
	FTransaction Retry(43, {.Description = "Valid retry"});
	Retry.AddRecord(std::make_unique<FTestCustomChange>(Value, 1, 2));
	const auto Applied = Retry.Apply(false, Durin::EPropertyChangeOrigin::Redo);
	ASSERT_TRUE(Applied);
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Rejected.error().Message, RejectionMessage);
}

TEST(FTransBufferTests, SuccessfulCompensationRestoresValuesAndAllowsRetry)
{
	using namespace Durin;
	using namespace Durin::Editor;
	int First = 1, Second = 2;
	FTransaction Transaction(43, {.Description = "Restored compensation"});
	Transaction.AddRecord(std::make_unique<FTestCustomChange>(First, 1, 10));
	auto FailingChange = std::make_unique<FTestCustomChange>(Second, 2, 20);
	auto* Change = FailingChange.get();
	Change->bFailRedo = true;
	Transaction.AddRecord(std::move(FailingChange));
	const auto Rejected = Transaction.Apply(false, EPropertyChangeOrigin::Redo);
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Status, ETransactionApplyFailure::Restored);
	EXPECT_EQ(Rejected.error().RecordIndex, 1u);
	EXPECT_TRUE(Rejected.error().RollbackFailures.empty());
	EXPECT_EQ(First, 1);
	EXPECT_EQ(Second, 2);
	Change->bFailRedo = false;
	const auto Applied = Transaction.Apply(false, EPropertyChangeOrigin::Redo);
	ASSERT_TRUE(Applied);
	EXPECT_EQ(First, 10);
	EXPECT_EQ(Second, 20);
	EXPECT_EQ(Rejected.error().Status, ETransactionApplyFailure::Restored);
}

TEST(FTransBufferTests, CustomInternalCompensationFailureRequiresRecovery)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "InternalCompensationTransBuffer"));
	TStrongObjectPtr<DTransactionRecordParticipant> Participant(NewObject<DTransactionRecordParticipant>(nullptr, "RecoveryParticipant"));
	TWeakObjectPtr<DObject> Weak(Participant.Get());
	int Value = 0;
	auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1, Participant.Get(), "RecoveryModule");
	Change->bFailRedo = true;
	Change->bFailCleanup = true;
	const auto Result = Durin::Editor::ApplyEditorChange(*Buffer, std::move(Change));
	EXPECT_EQ(Result.Code, ETransactorResultCode::RecoveryRequired);
	ASSERT_TRUE(Result.ApplyCause);
	EXPECT_EQ(Result.ApplyCause->Status, ETransactionApplyFailure::RecoveryRequired);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_FALSE(Buffer->CanTransact());
	const auto Drain = Buffer->DiscardCustomChangesByModule("RecoveryModule");
	EXPECT_EQ(Drain.Code, ETransactorResultCode::Rejected);
	ASSERT_TRUE(Drain.RejectionCause);
	EXPECT_EQ(Drain.RejectionCause->Reason, ETransactorRejectionReason::ModuleState);
	Participant.Reset();
	CollectGarbage();
	EXPECT_TRUE(Weak.IsValid());
	Buffer.Reset();
	CollectGarbage();
	EXPECT_FALSE(Weak.IsValid());
}

TEST(FTransBufferTests, ThrowingReplayReportsRecoveryAndRetainsItsParticipantGraph)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "ThrowingReplayRecovery"));
	TStrongObjectPtr<DTransactionRecordParticipant> Participant(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingReplayParticipant"));
	TWeakObjectPtr<DObject> Weak(Participant.Get());
	int Value = 0;
	auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1, Participant.Get());
	Change->bThrowRedo = true;
	const auto Result = Durin::Editor::ApplyEditorChange(*Buffer, std::move(Change));
	EXPECT_EQ(Result.Code, ETransactorResultCode::RecoveryRequired);
	ASSERT_TRUE(Result.ApplyCause);
	EXPECT_NE(Result.ApplyCause->Message.find("replay exception fixture"), std::string::npos);
	EXPECT_EQ(Value, 1);
	Participant.Reset();
	CollectGarbage();
	EXPECT_TRUE(Weak.IsValid());
	Buffer.Reset();
	CollectGarbage();
	EXPECT_FALSE(Weak.IsValid());
}

TEST(FTransBufferTests, OversizedHistoryIsTrimmedOnlyBeforeTheNextSuccessfulCommit)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Measurement;
	int MeasurementValue = 0;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Measurement, std::make_unique<FTestCustomChange>(MeasurementValue, 0, 1)));
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "SoftBudgetTransBuffer"));
	FTransBufferTestAccess::InitializeMemoryBudget(*Buffer, Measurement->GetOwnedBytes() + 1);
	int Value = 0;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	auto Growing = std::make_unique<FTestCustomChange>(Value, 1, 2);
	Growing->bGrowOnRedo = true;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::move(Growing)));
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_GT(Buffer->GetOwnedBytes(), Buffer->GetMemoryBudget());
	const auto Head = Buffer->GetUndoId();
	const auto Bytes = Buffer->GetOwnedBytes();
	const auto Canceled = BeginRecording(*Buffer, {.Description = "Canceled over-budget edit"});
	ASSERT_TRUE(Canceled);
	EXPECT_EQ(Buffer->GetUndoId(), Head);
	EXPECT_EQ(CancelRecording(*Buffer, Canceled.Index).Code, ETransactorResultCode::Discarded);
	const auto NoOp = BeginRecording(*Buffer, {.Description = "No-op over-budget edit"});
	ASSERT_TRUE(NoOp);
	EXPECT_EQ(EndRecording(*Buffer).Code, ETransactorResultCode::NoOp);
	auto Failing = std::make_unique<FTestCustomChange>(Value, 2, 3);
	Failing->bFailRedo = true;
	EXPECT_FALSE(Durin::Editor::ApplyEditorChange(*Buffer, std::move(Failing)));
	EXPECT_EQ(Buffer->GetOwnedBytes(), Bytes);
	EXPECT_EQ(Buffer->GetUndoId(), Head);
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 2);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 2, 3)));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 2);
}

TEST(FTransBufferTests, ByteBudgetDoesNotImposeAnEntryCountLimit)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	for (int Index = 0; Index < 300; ++Index)
		ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, Index, Index + 1)));
	EXPECT_EQ(Buffer->GetQueueLength(), 300u);
	EXPECT_LT(Buffer->GetOwnedBytes(), Buffer->GetMemoryBudget());
	for (int Index = 0; Index < 300; ++Index) ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 0);
	for (int Index = 0; Index < 300; ++Index) ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 300);
}

TEST(FTransBufferTests, OldHistoryAtTheByteBudgetIsTrimmedBeforeAppending)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Measurement;
	int Value = 0;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Measurement, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	Tests::FTestTransactorOwner Buffer("ExactByteBudget", Measurement->GetOwnedBytes());
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
	EXPECT_EQ(Buffer->GetOwnedBytes(), Buffer->GetMemoryBudget());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 2, 3)));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 2);
}

TEST(FTransBufferTests, RedoBranchReplacementDoesNotAlsoTrimTheUndoPrefix)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Measurement;
	int MeasurementValue = 0;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Measurement, std::make_unique<FTestCustomChange>(MeasurementValue, 0, 1)));
	Tests::FTestTransactorOwner Buffer("OverBudgetRedo", Measurement->GetOwnedBytes() + 1);
	int Value = 0;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	auto Growing = std::make_unique<FTestCustomChange>(Value, 1, 2);
	Growing->bGrowOnRedo = true;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::move(Growing)));
	ASSERT_GT(Buffer->GetOwnedBytes(), Buffer->GetMemoryBudget());
	ASSERT_TRUE(Buffer->Undo());
	const auto UndoId = Buffer->GetUndoId();
	const auto RedoId = Buffer->GetRedoId();
	const auto Canceled = BeginRecording(*Buffer, {.Description = "Canceled branch"});
	ASSERT_TRUE(Canceled);
	EXPECT_EQ(CancelRecording(*Buffer, Canceled.Index).Code, ETransactorResultCode::Discarded);
	EXPECT_EQ(Buffer->GetUndoId(), UndoId);
	EXPECT_EQ(Buffer->GetRedoId(), RedoId);
	(void)ReadTransactionEvents(*Buffer);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 3)));
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	const auto Events = ReadTransactionEvents(*Buffer);
	EXPECT_TRUE(std::ranges::none_of(Events, [](const auto& Event) { return Event.Type == ETransactionEventType::Evicted; }));
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 0);
}

TEST(FTransBufferTests, NativeBeginEndAndInnerCancelUseUEActiveCounts)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CancelCounts"));
	EXPECT_EQ(Buffer->Begin("test", "Outer"), 0);
	ASSERT_TRUE(Target->Modify()); Target->Value = 1;
	const auto Inner = Buffer->Begin("test", "Inner"); EXPECT_EQ(Inner, 1);
	ASSERT_TRUE(Target->Modify()); Target->Value = 2;
	Buffer->Cancel(Inner);
	EXPECT_FALSE(Buffer->IsActive()); EXPECT_EQ(Buffer->End(), 0);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_EQ(Target->Value, 2);
	EXPECT_EQ(GUndo, nullptr);
	EXPECT_EQ(Buffer->Begin("test", "Next"), 0);
	EXPECT_EQ(Buffer->Begin("test", "Nested"), 1);
	EXPECT_EQ(Buffer->End(), 2); EXPECT_TRUE(Buffer->IsActive());
	EXPECT_EQ(Buffer->End(), 1); EXPECT_FALSE(Buffer->IsActive());
}

TEST(FTransBufferTests, TransientAndNonTransactionalFiltersAreIndependent)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "TransactionFilters"));
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Filters"});
	  ASSERT_TRUE(Target->Modify()); Target->SessionValue = 10; Target->UnrecordedValue = 20; }
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->SessionValue, 0); EXPECT_EQ(Target->UnrecordedValue, 20);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->SessionValue, 10);
}

TEST(FTransBufferTests, HistoricalRestoreDoesNotRunNewEditValidation)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ReplayValidation"));
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Original edit"}); ASSERT_TRUE(Target->Modify()); Target->Value = 17; }
	Target->bRejectNewEdits = true;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 17);
	EXPECT_EQ(Target->ValidationCalls, 0);
}

TEST(FTransBufferTests, ModifyMarksDirtyWithoutRequiringAnActiveRecording)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "ModifyDirty"));
	(void)MakeSoftPath(); FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ModifyDirty", PackagePath)); Package->InitializeAssetPackage(PackagePath);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Target"));
	Package->ClearDirty(); EXPECT_FALSE(Target->Modify(false)); EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(Target->Modify()); EXPECT_TRUE(Package->IsDirty());
	Package->ClearDirty(); Target->ClearFlags(EObjectFlags::Transactional);
	Tests::FTestTransactorOwner Buffer; EXPECT_EQ(Buffer->Begin("test", "Unrecorded"), 0);
	EXPECT_FALSE(Target->Modify()); Target->Value = 5; EXPECT_EQ(Buffer->End(), 1);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_TRUE(Package->IsDirty());
}

TEST(FTransBufferTests, IndependentListenersReceiveTheSamePostTransitionEvents)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	std::vector<FTransactionEvent> First, Second;
	const auto A = Buffer->AddTransactionListener([&](const auto& Event) { First.push_back(Event); EXPECT_FALSE(Buffer->IsActive()); });
	const auto B = Buffer->AddTransactionListener([&](const auto& Event) { Second.push_back(Event); });
	int Value = 0; ASSERT_TRUE(ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	EXPECT_TRUE(First.empty()); Buffer->DispatchEvents();
	ASSERT_EQ(First.size(), 1u); ASSERT_EQ(Second.size(), 1u); EXPECT_EQ(First[0].Id, Second[0].Id);
	Buffer->RemoveTransactionListener(A); ASSERT_TRUE(Buffer->Undo()); Buffer->DispatchEvents();
	EXPECT_EQ(First.size(), 1u); EXPECT_EQ(Second.size(), 2u); Buffer->RemoveTransactionListener(B);
}

TEST(FTransBufferTests, CustomOwnerSuppressesLaterModifySnapshot)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CustomOwner"));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Custom owner"});
		Target->Value = 1;
		ASSERT_TRUE(Buffer->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 0, 1)));
		ASSERT_TRUE(Target->Modify()); Target->Value = 2;
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 1);
}

TEST(FTransBufferTests, ModifyBeforeCustomPreservesOrderedReplay)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(
		NewObject<DTransactionRecordParticipant>(nullptr, "ModifyBeforeCustom"));
	std::vector<int32> ValuesBeforeCustomReplay;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Serialized then custom"});
		ASSERT_TRUE(Target->Modify());
		Target->Value = 1;
		auto Change = std::make_unique<FTestCustomChange>(Target->Value, 1, 2);
		Change->ReplayProbe = [&] { ValuesBeforeCustomReplay.push_back(Target->Value); };
		Target->Value = 2;
		ASSERT_TRUE(Buffer->StoreUndo(Target.Get(), std::move(Change)));
	}
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 2);
	EXPECT_EQ(ValuesBeforeCustomReplay, (std::vector<int32>{2, 2}));
}

TEST(FTransBufferTests, DisabledScopeLeavesOuterCaptureAndLifetimeIntact)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(
		NewObject<DTransactionRecordParticipant>(nullptr, "DisabledNestedScope"));
	{
		FScopedTransaction Outer(Buffer.Get(), {"test", "Outer"});
		auto* Recording = GUndo;
		{
			FScopedTransaction Disabled(Buffer.Get(), {"test", "Disabled"}, false);
			EXPECT_FALSE(Disabled.IsOutstanding());
			EXPECT_EQ(GUndo, Recording);
			ASSERT_TRUE(Target->Modify());
			Target->Value = 7;
			Disabled.Cancel();
			EXPECT_EQ(Disabled.End(), 0);
		}
		EXPECT_TRUE(Outer.IsOutstanding());
		EXPECT_EQ(GUndo, Recording);
	}
	EXPECT_EQ(GUndo, nullptr);
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 7);
}

TEST(FTransBufferTests, NoOpAndCanceledRecordingPreserveRedoBranch)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(
		NewObject<DTransactionRecordParticipant>(nullptr, "PreservedRedo"));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Original"});
		ASSERT_TRUE(Target->Modify());
		Target->Value = 5;
	}
	ASSERT_TRUE(Buffer->Undo());
	const auto RedoId = Buffer->GetRedoId();
	{
		FScopedTransaction NoOp(Buffer.Get(), {"test", "No-op"});
		ASSERT_TRUE(Target->Modify());
	}
	EXPECT_EQ(Buffer->GetRedoId(), RedoId);
	{
		FScopedTransaction Canceled(Buffer.Get(), {"test", "Canceled"});
		ASSERT_TRUE(Target->Modify());
		Target->Value = 3;
		Canceled.Cancel();
	}
	EXPECT_EQ(Target->Value, 3);
	EXPECT_EQ(Buffer->GetRedoId(), RedoId);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 5);
}

TEST(FTransBufferTests, UndoWithoutRedoDiscardsTheRedoSuffix)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Undo(false));
	EXPECT_EQ(Value, 0); EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_FALSE(Buffer->CanRedo());
}

TEST(FTransBufferTests, UnrecordedAndCanceledModifyCannotClearDirtyOnUndo)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "UnrecordedDirty"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/UnrecordedDirty", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Target"));
	Buffer->EstablishSavedState(*Package);
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Recorded"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
	EXPECT_FALSE(Target->Modify()); Target->UnrecordedValue = 9;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_TRUE(Package->IsDirty());
	Buffer->MarkSaved(*Package);
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Canceled"}); ASSERT_TRUE(Target->Modify()); Target->Value = 2; Scope.Cancel(); }
	EXPECT_EQ(Target->Value, 2); EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Package->IsDirty());
}

TEST(FTransBufferTests, PrimaryContextIsNotAnAffectedPackage)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> PrimaryPackage(NewObject<DPackage>(nullptr, "PrimaryContextPackage"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/PrimaryContext", Path)); PrimaryPackage->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Primary(NewObject<DTransactionRecordParticipant>(PrimaryPackage.Get(), "Primary"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ActualTarget"));
	Buffer->EstablishSavedState(*PrimaryPackage);
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Unrelated context", Primary.Get()});
		ASSERT_TRUE(Target->Modify()); Target->Value = 1;
	}
	EXPECT_FALSE(PrimaryPackage->IsDirty());
	const auto* Transaction = Buffer->GetTransaction(0); ASSERT_NE(Transaction, nullptr);
	EXPECT_EQ(Transaction->GetContext().PrimaryObject, Primary.Get());
	EXPECT_EQ(Transaction->GetTransactionObjects(), std::vector<DObject*>{Target.Get()});
	ASSERT_TRUE(Buffer->Undo()); EXPECT_FALSE(PrimaryPackage->IsDirty());
}

TEST(FTransBufferTests, TransactionFiltersApplyInsideReflectedStructs)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "NestedFieldFilters"));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Nested field filters"});
		ASSERT_TRUE(Target->Modify()); Target->Nested.Count = 1; Target->Nested.Session = 2; Target->Nested.Ignored = 3;
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Nested.Count, 0); EXPECT_EQ(Target->Nested.Session, 0); EXPECT_EQ(Target->Nested.Ignored, 3);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Nested.Count, 1); EXPECT_EQ(Target->Nested.Session, 2); EXPECT_EQ(Target->Nested.Ignored, 3);
	const auto Count = Buffer->GetQueueLength();
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Excluded field only"}); ASSERT_TRUE(Target->Modify()); Target->Nested.Ignored = 7; }
	EXPECT_EQ(Buffer->GetQueueLength(), Count);
}

TEST(FTransBufferTests, ExcludedStructReferencesDoNotAcquireHistoryRetention)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ExcludedReferenceOwner"));
	auto* Referenced = NewObject<DObject>(nullptr, "ExcludedHistoryReference");
	TWeakObjectPtr<DObject> Weak(Referenced);
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Excluded reference"}); ASSERT_TRUE(Target->Modify());
		Target->Nested.Count = 1; Target->Nested.IgnoredReference = Referenced;
	}
	Target->Nested.IgnoredReference = nullptr; CollectGarbage(); EXPECT_FALSE(Weak.IsValid());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Nested.Count, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Nested.Count, 1);
}

TEST(FTransBufferTests, InvalidInitialAccountingRejectsBeforeMutation)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0; auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1); Change->bOverflowed = true;
	const auto Result = ApplyEditorChange(*Buffer, std::move(Change));
	EXPECT_EQ(Result.Code, ETransactorResultCode::Failed); EXPECT_EQ(Value, 0); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_TRUE(Buffer->CanTransact());
}

TEST(FTransBufferTests, InvalidReplayAccountingPreservesCursorAndRequiresRecovery)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0; auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1); auto* Retained = Change.get();
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::move(Change))); ASSERT_TRUE(Buffer->Undo());
	Retained->bOverflowOnRedo = true;
	const auto Id = Buffer->GetRedoId(); EXPECT_FALSE(Buffer->Redo());
	EXPECT_EQ(Buffer->GetLastResult().Code, ETransactorResultCode::RecoveryRequired);
	EXPECT_EQ(Buffer->GetRedoId(), Id); EXPECT_EQ(Buffer->GetUndoId(), 0u); EXPECT_FALSE(Buffer->CanTransact());
	EXPECT_EQ(Value, 1); EXPECT_EQ(GUndo, nullptr);
}

TEST(FTransBufferTests, ModuleRetirementCannotEraseAReplayingRecord)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0; auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1, nullptr, "ReplayOwnedModule"); auto* Retained = Change.get();
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::move(Change)));
	Retained->ReplayProbe = [&] {
		EXPECT_EQ(Buffer->DiscardCustomChangesByModule("ReplayOwnedModule").Code, ETransactorResultCode::Rejected);
		EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	};
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Value, 0);
	ASSERT_TRUE(Buffer->DiscardCustomChangesByModule("ReplayOwnedModule")); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
}

TEST(FTransBufferTests, GeneratedReferencesRemainOwnedByConstruction)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "GeneratedReferenceOwner"));
	auto* Before = NewObject<DObject>(nullptr, "BeforeGenerated", EObjectConstructionPurpose::Generated);
	auto* After = NewObject<DObject>(nullptr, "AfterGenerated", EObjectConstructionPurpose::Generated);
	Target->Hard = Before;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Generated reference"}); ASSERT_TRUE(Target->Modify());
		Target->Value = 1; Target->Hard = After; MarkObjectHierarchyAsGarbage(Before);
	}
	CollectGarbage(); ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->Hard.Get(), After);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 1); EXPECT_EQ(Target->Hard.Get(), After);
	ASSERT_TRUE(Buffer->Reset()); std::string Reason; EXPECT_FALSE(Buffer->CanUndo(&Reason)); EXPECT_FALSE(Reason.empty());
}

TEST(FTransBufferTests, StaleScopesCannotEndOrCancelReplacementRecording)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	for (const bool CancelOld : {false, true})
	{
		FScopedTransaction Old(Buffer.Get(), {.Description = "Old"});
		if (CancelOld) Buffer->Cancel(); else EXPECT_EQ(1, Buffer->End());
		FScopedTransaction Replacement(Buffer.Get(), {.Description = "Replacement"});
		const auto Id = Buffer->GetLastResult().TransactionId;
		EXPECT_FALSE(Old.IsOutstanding());
		if (CancelOld) Old.Cancel(); else EXPECT_EQ(0, Old.End());
		EXPECT_EQ(Id, Buffer->GetLastResult().TransactionId);
		EXPECT_TRUE(Replacement.IsOutstanding());
		EXPECT_EQ(1, Replacement.End());
	}
}

TEST(FTransBufferTests, DestroyedTransactorInvalidatesRetainedScopeBeforeCleanup)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	FScopedTransaction Scope(Buffer.Get(), {.Description = "Destroyed"});
	EXPECT_TRUE(Scope.IsOutstanding());
	Buffer->BeginDestroy();
	EXPECT_FALSE(Scope.IsOutstanding());
	EXPECT_EQ(0, Scope.End());
	EXPECT_EQ(nullptr, GUndo);
}

TEST(FTransBufferTests, OnlyOneBufferCanPublishTheCurrentRecording)
{
    using namespace Durin;
    using namespace Durin::Editor;
    InitializeDObjectSystem(); Tests::FTestTransactorOwner First("FirstRecording"), Second("SecondRecording");
    TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SingleRecordingTarget"));
    FScopedTransaction Scope(First.Get(), {.Description = "First"});
    auto* Current = GUndo;
    ASSERT_NE(Current, nullptr);
    EXPECT_EQ(Second->Begin("test", "Competing"), -1);
    EXPECT_FALSE(Second->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 0, 1)));
    EXPECT_EQ(Second->GetQueueLength(), 0u);
    EXPECT_EQ(GUndo, Current);
    ASSERT_TRUE(Target->Modify()); Target->Value = 2;
    EXPECT_TRUE(Current->ContainsObject(Target.Get()));
    EXPECT_EQ(Scope.End(), 1);
    EXPECT_EQ(GUndo, nullptr);
    FScopedTransaction Next(Second.Get(), {.Description = "Second"});
    EXPECT_TRUE(Next.IsOutstanding());
    EXPECT_NE(GUndo, nullptr);
    Next.Cancel();
    EXPECT_EQ(GUndo, nullptr);
    ASSERT_TRUE(First->Undo()); EXPECT_EQ(Target->Value, 0);
}

TEST(FTransBufferTests, ContextQueriesFollowCursorAndAvailabilityThroughTheBaseInterface)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer("ContextQueries");
	DTransactor* History = Buffer.Get();
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ContextQueryTarget"));
	auto Record = [&](std::string_view Description, int Value) {
		FScopedTransaction Scope(Buffer.Get(), {"ContextQuery", std::string(Description), Target.Get()});
		EXPECT_TRUE(Scope.IsOutstanding());
		EXPECT_TRUE(Target->Modify());
		Target->Value = Value;
	};
	EXPECT_TRUE(History->GetUndoContext().Description.empty());
	EXPECT_TRUE(History->GetRedoContext().Description.empty());
	Record("First", 1);
	Record("Second", 2);
	const auto SavedContext = History->GetUndoContext();
	EXPECT_EQ(SavedContext.Name, "ContextQuery");
	EXPECT_EQ(SavedContext.Description, "Second");
	EXPECT_EQ(SavedContext.PrimaryObject, Target.Get());
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(History->GetUndoContext().Description, "First");
	EXPECT_EQ(History->GetRedoContext().Description, "Second");
	{
		FScopedTransaction Pending(Buffer.Get(), {.Description = "Pending"});
		EXPECT_TRUE(History->GetUndoContext().Description.empty());
		EXPECT_EQ(History->GetUndoContext(false).Description, "First");
		EXPECT_TRUE(History->GetRedoContext().Description.empty());
		Pending.Cancel();
	}
	EXPECT_EQ(History->GetRedoContext().Description, "Second");
	ASSERT_TRUE(History->Redo());
	EXPECT_EQ(History->GetUndoContext().Description, "Second");
	ASSERT_TRUE(History->Undo());
	Record("Branch", 3);
	EXPECT_EQ(History->GetUndoContext().Description, "Branch");
	EXPECT_TRUE(History->GetRedoContext().Description.empty());
	EXPECT_EQ(History->GetQueueLength(), 2u);
	ASSERT_TRUE(History->Reset());
	EXPECT_TRUE(History->GetUndoContext(false).Description.empty());
	EXPECT_TRUE(History->GetRedoContext().Description.empty());
	EXPECT_EQ(SavedContext.Description, "Second");
}

TEST(FTransBufferTests, EditorConvenienceScopesExposeContextsAndRetireBeforeReplacement)
{
    using namespace Durin;
    using namespace Durin::Editor;
    InitializeDObjectSystem();
    TStrongObjectPtr<DEditorEngine> First(NewObject<DEditorEngine>(nullptr, "FirstEditorLifetime"));
    ASSERT_TRUE(First->ActivateEditor());
    TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "EditorContextTarget"));
    {
        FScopedTransaction Scope("test", "Editor edit", Target.Get());
        ASSERT_TRUE(Scope.IsOutstanding());
        ASSERT_TRUE(Target->Modify()); Target->Value = 3;
    }
    auto* Buffer = First->GetTransactor();
    EXPECT_EQ(Buffer->GetQueueLength(), 1u);
    EXPECT_EQ(Buffer->GetUndoContext().Description, "Editor edit");
    EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Target.Get());
    ASSERT_TRUE(Buffer->Undo());
    EXPECT_EQ(Buffer->GetUndoCount(), 1u);
    EXPECT_EQ(Buffer->GetRedoContext().Description, "Editor edit");
    ASSERT_TRUE(Buffer->Redo());
    FScopedTransaction Stale("Before shutdown");
    ASSERT_TRUE(Stale.IsOutstanding());
    First->PrepareForShutdown();
    EXPECT_EQ(GEditor, nullptr); EXPECT_EQ(GUndo, nullptr);
    EXPECT_FALSE(Stale.IsOutstanding());
    TStrongObjectPtr<DEditorEngine> Second(NewObject<DEditorEngine>(nullptr, "SecondEditorLifetime"));
    ASSERT_TRUE(Second->ActivateEditor());
    FScopedTransaction Next("After shutdown");
    ASSERT_TRUE(Next.IsOutstanding());
    auto* Current = GUndo;
    EXPECT_EQ(Stale.End(), 0);
    EXPECT_EQ(GUndo, Current);
    Next.Cancel(); Second->PrepareForShutdown();
    EXPECT_EQ(GUndo, nullptr); EXPECT_EQ(GEditor, nullptr);
}

TEST(FTransBufferTests, HostDestructionInvalidatesSuspendedScopeAndReleasesGcReferences)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DEditorEngine> Host(NewObject<DEditorEngine>(nullptr, "SuspendedScopeHost"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SuspendedScopeTarget"));
	TWeakObjectPtr<DTransactionRecordParticipant> WeakTarget(Target.Get());
	FScopedTransaction Scope(Host->GetTransactor(), {.Description = "Across collection"});
	Host->DispatchSubsystemCallback([&] { ASSERT_TRUE(Target->Modify()); Target->Value = 1; });
	Target.Reset(); CollectGarbage();
	EXPECT_NE(nullptr, WeakTarget.Get());
	EXPECT_TRUE(Scope.IsOutstanding());
	MarkObjectHierarchyAsGarbage(Host.Get()); Host.Reset(); CollectGarbage();
	EXPECT_FALSE(Scope.IsOutstanding());
	EXPECT_EQ(0, Scope.End());
	EXPECT_EQ(nullptr, WeakTarget.Get());
	EXPECT_EQ(nullptr, GUndo);
}
