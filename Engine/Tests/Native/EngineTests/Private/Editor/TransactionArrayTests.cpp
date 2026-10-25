#include "Editor/TransactionRecordTestSupport.h"
#include "DObject/TransactionArray.h"
#include "DObject/TransactionSerialization.h"
#include "Components/SplineComponent.h"

using namespace Durin;
using namespace Durin::Editor;
using namespace Durin::Tests::ObjectState;

namespace
{
	auto EditNumbers(DTransactionRecordParticipant& Object, size_t Index, size_t Removed, std::initializer_list<int32> Values) -> FTransactionCaptureResult
	{
		return EditTransactionArray(Object, Object.GetClass()->FindPropertyByName("Numbers"), Object.Numbers,
			Index, Removed, std::span<const int32>(Values.begin(), Values.size()));
	}
}

TEST(FTransactionArrayTests, PieOwnerIsRejectedBeforeMutation)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "PieArray"));
	Package->SetPlayInEditor(true);
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(Package.Get(), "Owner"));
	Object->Numbers = {1, 2, 3};
	{ FScopedTransaction Scope(Buffer.Get(), {.Description = "PIE array"}); EXPECT_FALSE(EditNumbers(*Object, 0, 1, {})); }
	EXPECT_EQ(Object->Numbers, (std::vector<int32>{1, 2, 3}));
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
}

TEST(FTransactionArrayTests, ReplaysOrderedOverlappingOperationsAcrossRelocation)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "RangeOperations"));
	Object->Numbers = {1, 2, 3, 4};
	{
		FScopedTransaction Outer(Buffer.Get(), {.Description = "Array operations"});
		ASSERT_TRUE(EditNumbers(*Object, 1, 1, {}));
		FScopedTransaction Inner(Buffer.Get(), {.Description = "Nested array operations"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 0, {8, 9}));
		Object->Numbers.reserve(10000);
		ASSERT_TRUE(EditNumbers(*Object, 2, 1, {7}));
		ASSERT_TRUE(EditNumbers(*Object, 1, 2, {}));
	}
	EXPECT_EQ(Object->Numbers, (std::vector<int32>{8, 3, 4}));
	ASSERT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 4u);
	EXPECT_EQ(Object->SerializationSaves, 0);
	for (int Repeat = 0; Repeat != 3; ++Repeat)
	{
		ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Object->Numbers, (std::vector<int32>{1, 2, 3, 4}));
		ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Object->Numbers, (std::vector<int32>{8, 3, 4}));
	}
	EXPECT_EQ(Object->UndoCompletions, 6);
}

TEST(FTransactionArrayTests, RejectsInvalidRangesAndSkipsNoOpAdapters)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "RangeValidation"));
	Object->Numbers = {1, 2};
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "No changes"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {1}));
		ASSERT_TRUE(EditNumbers(*Object, 2, 0, {}));
		EXPECT_FALSE(EditNumbers(*Object, std::numeric_limits<size_t>::max(), 1, {}));
		EXPECT_FALSE(EditNumbers(*Object, 1, std::numeric_limits<size_t>::max(), {}));
		EXPECT_FALSE(EditNumbers(*Object, 0, 1, {3, 4}));
		EXPECT_FALSE(GUndo->SaveArray({.Object = Object.Get(), .Member = Object->GetClass()->FindPropertyByName("Numbers"),
			.Index = 0, .Count = std::numeric_limits<size_t>::max(), .Operation = ETransactionArrayOperation::Insert}));
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_EQ(Object->Numbers, (std::vector<int32>{1, 2}));
}

TEST(FTransactionArrayTests, WholeObjectFallbackAndRangeBaselinesRemainOrdered)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "MixedRangeSnapshots"));
	Object->Numbers = {1, 2, 3};
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Range then whole object"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {}));
		ASSERT_TRUE(Object->Modify(false));
		ASSERT_TRUE(EditNumbers(*Object, 1, 0, {8}));
		Object->Value = 12;
	}
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 2u);
	for (int Repeat = 0; Repeat != 2; ++Repeat)
	{
		ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Object->Numbers, (std::vector<int32>{1, 2, 3})); EXPECT_EQ(Object->Value, 0);
		ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Object->Numbers, (std::vector<int32>{2, 8, 3})); EXPECT_EQ(Object->Value, 12);
	}
}

TEST(FTransactionArrayTests, FailedPeerCompensatesRangesWithoutPublishingFlips)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "RangeCompensation"));
	Object->Numbers = {1, 2, 3};
	auto Change = std::make_unique<FTestCustomChange>(Object->Value, 0, 4);
	auto* Failure = Change.get(); Failure->bFailUndo = true;
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Range compensation"});
		Object->Value = 4;
		ASSERT_TRUE(Buffer->StoreUndo(nullptr, std::move(Change)));
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {}));
		ASSERT_TRUE(EditNumbers(*Object, 1, 0, {8}));
	}
	EXPECT_FALSE(Buffer->Undo());
	EXPECT_EQ(Object->Numbers, (std::vector<int32>{2, 8, 3}));
	Failure->bFailUndo = false;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Object->Numbers, (std::vector<int32>{1, 2, 3}));
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Object->Numbers, (std::vector<int32>{2, 8, 3}));
}

TEST(FTransactionArrayTests, RetainsNestedHardEdgesButNotWeakEdgesAndPreservesOmittedValues)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "NestedRangeReferences"));
	TStrongObjectPtr<DObject> Hard(NewObject<DObject>(nullptr, "RangeHard")), Weak(NewObject<DObject>(nullptr, "RangeWeak"));
	const FObjectKey HardKey(Hard.Get()), WeakKey(Weak.Get());
	Object->NestedArray.resize(1); Object->NestedArray[0].Count = 7;
	Object->NestedArray[0].HardReference = Hard.Get(); Object->NestedArray[0].WeakReference = Weak.Get();
	const auto* Member = Object->GetClass()->FindPropertyByName("NestedArray");
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Remove referenced range"});
		ASSERT_TRUE(EditTransactionArray(*Object, Member, Object->NestedArray, 0, 1, std::span<const FRecordNestedValue>{}));
	}
	Hard = nullptr; Weak = nullptr; CollectGarbage();
	EXPECT_NE(ResolveObjectKey(HardKey), nullptr); EXPECT_EQ(ResolveObjectKey(WeakKey), nullptr);
	ASSERT_TRUE(Buffer->Undo()); ASSERT_EQ(Object->NestedArray.size(), 1u);
	EXPECT_EQ(Object->NestedArray[0].HardReference.Get(), ResolveObjectKey(HardKey));
	EXPECT_EQ(Object->NestedArray[0].WeakReference.Get(), nullptr);
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Replace nested value"});
		FRecordNestedValue Replacement = Object->NestedArray[0]; Replacement.Count = 42;
		ASSERT_TRUE(EditTransactionArray(*Object, Member, Object->NestedArray, 0, 1, std::span<const FRecordNestedValue>(&Replacement, 1)));
	}
	Object->NestedArray[0].Ignored = 99;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Object->NestedArray[0].Count, 7); EXPECT_EQ(Object->NestedArray[0].Ignored, 99);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Object->NestedArray[0].Count, 42); EXPECT_EQ(Object->NestedArray[0].Ignored, 99);
}

TEST(FTransactionArrayTests, NestedSplineAdapterRestoresStableIdsAndEvaluation)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DSplineComponent> Spline(NewObject<DSplineComponent>(nullptr, "RangeSpline", EObjectFlags::Transactional));
	const auto Before = Spline->GetSplinePoints();
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Spline point edits"});
		ASSERT_TRUE(Spline->InsertSplinePoint(1, FSplinePoint({100, 200, 0})));
		ASSERT_TRUE(Spline->RemoveSplinePoint(0));
		ASSERT_TRUE(Spline->MoveSplinePoint(0, 1));
	}
	const auto After = Spline->GetSplinePoints();
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 3u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Spline->GetSplinePoints(), Before);
	EXPECT_EQ(Spline->GetEvaluationData()->GetNumSegments(), Spline->GetNumSplineSegments());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Spline->GetSplinePoints(), After);
	EXPECT_EQ(Spline->GetEvaluationData()->GetNumSegments(), Spline->GetNumSplineSegments());
}

TEST(FTransactionArrayTests, CancellationAndBoundedEvictionReleaseRangeOwners)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer("RangeBudget", 8192);
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "RangeEviction"));
	Object->Numbers.resize(100000, 5);
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Discard array recording"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {}));
		Scope.Cancel();
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	// Cancel discards recording; like other native adapters, it does not undo edits.
	EXPECT_EQ(Object->Numbers.size(), 99999u);
	for (int Index = 0; Index != 12; ++Index)
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Bounded range history"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {}));
	}
	EXPECT_LT(Buffer->GetQueueLength(), 12u);
	size_t EntryBytes = 0; ASSERT_TRUE(Buffer->GetTransaction(Buffer->GetQueueLength() - 1)->TryGetOwnedSize(EntryBytes));
	EXPECT_LT(Buffer->GetUndoSize(), Buffer->GetMemoryBudget() + EntryBytes);
	const FObjectKey OwnerKey(Object.Get()); Object = nullptr;
	CollectGarbage(); EXPECT_NE(ResolveObjectKey(OwnerKey), nullptr);
	ASSERT_TRUE(Buffer->TryReset()); CollectGarbage(); EXPECT_EQ(ResolveObjectKey(OwnerKey), nullptr);
}

TEST(FTransactionArrayTests, ArrayAnnotationsAndEventsUseTheCompletedSideOncePerOwner)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "ArrayAnnotations"));
	Object->Numbers = {1, 2, 3}; Object->bAnnotationEnabled = true; Object->SupplementalValue = 1;
	std::vector<FTransactionObjectEvent> Events;
	Object->TransactionObserver = [&](const FTransactionObjectEvent& Event) { Events.push_back(Event); };
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Annotated array operations"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {})); Object->SupplementalValue = 2;
		ASSERT_TRUE(EditNumbers(*Object, 1, 0, {8})); Object->SupplementalValue = 3;
	}
	ASSERT_EQ(Events.size(), 1u); EXPECT_EQ(Events.back().Change.ChangedProperties, (std::vector<FName>{"Numbers"}));
	ASSERT_TRUE(Events.back().Annotation); EXPECT_EQ(static_cast<FRecordTestAnnotation&>(*Events.back().Annotation).Value, 3);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Object->SupplementalValue, 1);
	ASSERT_EQ(Events.size(), 2u); EXPECT_EQ(static_cast<FRecordTestAnnotation&>(*Events.back().Annotation).Value, 1);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Object->SupplementalValue, 3);
	ASSERT_EQ(Events.size(), 3u); EXPECT_EQ(static_cast<FRecordTestAnnotation&>(*Events.back().Annotation).Value, 3);
}

TEST(FTransactionArrayTests, ReinstancingRebindsOwnerAndNestedHardAndWeakReferences)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Old(NewObject<DTransactionRecordParticipant>(nullptr, "OldArrayOwner"));
	TStrongObjectPtr<DTransactionRecordParticipant> New(NewObject<DTransactionRecordParticipant>(nullptr, "NewArrayOwner"));
	TStrongObjectPtr<DObject> OldReference(NewObject<DObject>(nullptr, "OldArrayReference"));
	TStrongObjectPtr<DObject> NewReference(NewObject<DObject>(nullptr, "NewArrayReference"));
	Old->NestedArray.resize(1); Old->NestedArray[0].Count = 7;
	Old->NestedArray[0].HardReference = OldReference.Get(); Old->NestedArray[0].WeakReference = OldReference.Get();
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Reinstance array data"});
		ASSERT_TRUE(EditTransactionArray(*Old, Old->GetClass()->FindPropertyByName("NestedArray"), Old->NestedArray, 0, 1, std::span<const FRecordNestedValue>{}));
	}
	const FObjectReplacementMap::FEntry Entries[]{{Old.Get(), New.Get()}, {OldReference.Get(), NewReference.Get()}};
	FObjectReplacementMap Map; ASSERT_TRUE(Map.BuildFromObjects(Entries));
	ASSERT_TRUE(Buffer->OnObjectsReinstanced(Map));
	MarkAsGarbage(Old.Get()); MarkAsGarbage(OldReference.Get()); CollectGarbage();
	ASSERT_TRUE(Buffer->Undo()); ASSERT_EQ(New->NestedArray.size(), 1u);
	EXPECT_EQ(New->NestedArray[0].Count, 7);
	EXPECT_EQ(New->NestedArray[0].HardReference.Get(), NewReference.Get());
	EXPECT_EQ(New->NestedArray[0].WeakReference.Get(), NewReference.Get());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(New->NestedArray.empty());
}

TEST(FTransactionArrayTests, DefaultGraphElementReferencesResolveRecreatedDefaultsWithoutRevivingWeakEdges)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	auto* Class = DTransactionRecordParticipant::StaticClass();
	const std::array Classes{Class}; ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	auto* Default = const_cast<DObject*>(static_cast<const DObject*>(Class->GetDefaultObject()));
	ASSERT_NE(Default, nullptr); const FObjectKey OldKey(Default);
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "DefaultReferenceArray"));
	Object->NestedArray.resize(1); Object->NestedArray[0].HardReference = Default; Object->NestedArray[0].WeakReference = Default;
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Default references in array elements"});
		ASSERT_TRUE(EditTransactionArray(*Object, Object->GetClass()->FindPropertyByName("NestedArray"), Object->NestedArray,
			0, 1, std::span<const FRecordNestedValue>{}));
	}
	Private::ReleaseClassDefaultObjectForTests(Class); CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	const auto* Replacement = Class->GetDefaultObject(); ASSERT_NE(Replacement, nullptr);
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	ASSERT_EQ(Object->NestedArray.size(), 1u);
	EXPECT_EQ(Object->NestedArray[0].HardReference.Get(), Replacement);
	EXPECT_EQ(Object->NestedArray[0].WeakReference.Get(), nullptr);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Object->NestedArray.empty());
}

TEST(FTransactionArrayTests, EmptyArraysAndAliasedInsertionsKeepValidRanges)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "EmptyRange"));
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Empty and aliased arrays"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 0, {1, 2})); Object->Numbers.shrink_to_fit();
		ASSERT_TRUE(EditTransactionArray(*Object, Object->GetClass()->FindPropertyByName("Numbers"), Object->Numbers,
			2, 0, std::span<const int32>(Object->Numbers)));
		ASSERT_TRUE(EditNumbers(*Object, 0, 4, {})); ASSERT_TRUE(EditNumbers(*Object, 0, 0, {8}));
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_TRUE(Object->Numbers.empty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Object->Numbers, (std::vector<int32>{8}));
}

TEST(FTransactionArrayTests, FirstArrayEditPreservesACleanPackageCheckpoint)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "ArrayCheckpoint"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ArrayCheckpoint", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(Package.Get(), "ArrayOwner"));
	Object->Numbers = {1, 2}; Package->ClearDirty();
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "First array package edit"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {}));
	}
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Package->IsDirty());
}

TEST(FTransactionArrayTests, PostSpliceObserverFailureRequiresRecovery)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "ArrayRecovery"));
	Object->Numbers = {1, 2};
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Array observer failure"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {}));
	}
	Object->bThrowPropertyNotification = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_EQ(Object->Numbers, (std::vector<int32>{1, 2}));
	Object->bThrowPropertyNotification = false; EXPECT_FALSE(Buffer->TryReset());
}

TEST(FTransactionArrayTests, ExpiredOwnersRejectReplayBeforeHooks)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "ExpiredArrayOwner"));
	Object->Numbers = {1, 2};
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Expired owner"});
		ASSERT_TRUE(EditNumbers(*Object, 0, 1, {}));
	}
	EXPECT_EQ(Object->UndoBegins, 0);
	MarkAsGarbage(Object.Get());
	EXPECT_FALSE(Buffer->Undo());
}

TEST(FTransactionArrayTests, FixedRangeStorageAndReplayDoNotDependOnContainerLength)
{
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "RangeStorage"));
	const auto* Member = Object->GetClass()->FindPropertyByName("Numbers");
	for (auto Operation : {ETransactionArrayOperation::Insert, ETransactionArrayOperation::Remove, ETransactionArrayOperation::Replace})
	{
		size_t PreviousBytes = 0;
		for (size_t Num : {128u, 1024u})
		{
			Object->Numbers.assign(Num, 5);
			const size_t Index = Num / 2;
			auto Record = FTransactionArrayRecord::Capture({.Object = Object.Get(), .Member = Member,
				.Index = Index, .Count = 1, .Operation = Operation});
			ASSERT_TRUE(Record) << Record.error();
			size_t Bytes = 0; ASSERT_TRUE(Record->TryGetAllocatedSize(Bytes));
			if (PreviousBytes) EXPECT_EQ(Bytes, PreviousBytes);
			PreviousBytes = Bytes;
			if (Operation == ETransactionArrayOperation::Insert) Object->Numbers.insert(Object->Numbers.begin() + Index, 9);
			else if (Operation == ETransactionArrayOperation::Remove) Object->Numbers.erase(Object->Numbers.begin() + Index);
			else Object->Numbers[Index] = 9;
			const auto Edited = Object->Numbers;
			ASSERT_TRUE(Record->Apply(false, EPropertyChangeOrigin::Undo, nullptr)); Record->CommitReplay(); Record->ClearPreparedReplay();
			EXPECT_EQ(Object->Numbers, std::vector<int32>(Num, 5));
			ASSERT_TRUE(Record->Apply(false, EPropertyChangeOrigin::Redo, nullptr)); Record->CommitReplay(); Record->ClearPreparedReplay();
			EXPECT_EQ(Object->Numbers, Edited);
		}
	}
}

TEST(FTransactionArrayTests, ReviewPieArrayInverseCompensatesWithoutRetainingPlayObjects)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "PieArrayInverse")); Package->SetPlayInEditor(true);
	TStrongObjectPtr<DObject> Pie(NewObject<DObject>(Package.Get(), "Dependency"));
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "RangeOwner"));
	Object->NestedArray.resize(1); Object->NestedArray[0].Count = 7;
	const auto* Member = Object->GetClass()->FindPropertyByName("NestedArray");
	{ FScopedTransaction Scope(Buffer.Get(), {.Description = "Replace range"}); FRecordNestedValue Value; Value.Count = 42;
		ASSERT_TRUE(EditTransactionArray(*Object, Member, Object->NestedArray, 0, 1, std::span<const FRecordNestedValue>(&Value, 1))); }
	for (const bool bUndo : {true, false})
	{
		const auto Head = bUndo ? Buffer->GetUndoId() : Buffer->GetRedoId(); const auto Count = Object->NestedArray[0].Count;
		Object->NestedArray[0].HardReference = Pie.Get();
		EXPECT_FALSE(bUndo ? Buffer->Undo() : Buffer->Redo());
		ASSERT_EQ(Object->NestedArray.size(), 1u); EXPECT_EQ(Object->NestedArray[0].Count, Count);
		EXPECT_EQ(Object->NestedArray[0].HardReference.Get(), Pie.Get());
		EXPECT_EQ(bUndo ? Buffer->GetUndoId() : Buffer->GetRedoId(), Head);
		EXPECT_FALSE(Buffer->ContainsPieObjects());
		Object->NestedArray[0].HardReference = nullptr;
		ASSERT_TRUE(bUndo ? Buffer->Undo() : Buffer->Redo());
	}
}
TEST(FTransactionArrayTests, PieInverseRejectionRestoresMixedRecordsAndAnnotations)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> PiePackage(NewObject<DPackage>(nullptr, "MixedPieInverse")); PiePackage->SetPlayInEditor(true);
	TStrongObjectPtr<DObject> Pie(NewObject<DObject>(PiePackage.Get(), "Dependency"));
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(nullptr, "FirstRange"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(nullptr, "SecondRange"));
	First->NestedArray.resize(1); First->NestedArray[0].Count = 7;
	First->bAnnotationEnabled = Second->bAnnotationEnabled = true;
	First->SupplementalValue = 10; Second->SupplementalValue = 30; Second->Numbers = {1};
	const auto* Member = First->GetClass()->FindPropertyByName("NestedArray"); int Counter = 5;
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Mixed inverse rejection"});
		FRecordNestedValue Value; Value.Count = 42;
		ASSERT_TRUE(EditTransactionArray(*First, Member, First->NestedArray, 0, 1, std::span<const FRecordNestedValue>(&Value, 1)));
		First->SupplementalValue = 20;
		ASSERT_TRUE(EditNumbers(*Second, 0, 1, {9})); Second->SupplementalValue = 40;
		ASSERT_TRUE(Buffer->StoreUndo(std::make_unique<FTestCustomChange>(Counter, 0, 5)));
	}
	First->NestedArray[0].HardReference = Pie.Get(); const auto Head = Buffer->GetUndoId();
	int Events = 0; First->TransactionObserver = Second->TransactionObserver = [&](const auto&) { ++Events; };
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::Idle);
	EXPECT_EQ(Buffer->GetUndoId(), Head); EXPECT_EQ(Counter, 5); EXPECT_EQ(Second->Numbers, (std::vector<int32>{9}));
	EXPECT_EQ(First->NestedArray[0].Count, 42); EXPECT_EQ(First->NestedArray[0].HardReference.Get(), Pie.Get());
	EXPECT_EQ(First->SupplementalValue, 20); EXPECT_EQ(Second->SupplementalValue, 40); EXPECT_EQ(Events, 0);
	EXPECT_EQ(First->UndoBegins, 1); EXPECT_EQ(First->UndoCompletions, 1);
	EXPECT_EQ(Second->UndoBegins, 1); EXPECT_EQ(Second->UndoCompletions, 1);
	EXPECT_FALSE(Buffer->ContainsPieObjects());
	First->NestedArray[0].HardReference = nullptr; ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo());
}

TEST(FTransactionArrayTests, CompensationCompletionTaintRequiresRecovery)
{
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> OrdinaryPackage(NewObject<DPackage>(nullptr, "CompensationTaint"));
	TStrongObjectPtr<DObject> Ordinary(NewObject<DObject>(OrdinaryPackage.Get(), "Retained"));
	TStrongObjectPtr<DPackage> PiePackage(NewObject<DPackage>(nullptr, "RejectedInverse")); PiePackage->SetPlayInEditor(true);
	TStrongObjectPtr<DObject> Pie(NewObject<DObject>(PiePackage.Get(), "Dependency"));
	TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "Owner"));
	Object->NestedArray.resize(1); Object->NestedArray[0].HardReference = Ordinary.Get();
	const auto* Member = Object->GetClass()->FindPropertyByName("NestedArray");
	{
		FScopedTransaction Scope(Buffer.Get(), {.Description = "Tainted compensation"});
		auto Value = Object->NestedArray[0]; Value.Count = 42;
		ASSERT_TRUE(EditTransactionArray(*Object, Member, Object->NestedArray, 0, 1, std::span<const FRecordNestedValue>(&Value, 1)));
	}
	Object->NestedArray[0].HardReference = Pie.Get();
	Object->Trace = [&](std::string_view Event) { if (Event == "post") OrdinaryPackage->SetPlayInEditor(true); };
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_FALSE(Buffer->CanTransact()); EXPECT_EQ(Object->NestedArray[0].HardReference.Get(), Pie.Get());
}