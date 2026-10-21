#include "Editor/TransactionRecordTestSupport.h"

using namespace Durin::Tests::ObjectState;

TEST(FTransactionObjectRecordTests, CollectorTraversalRetainsTargetAndHardReferences)
{
	InitializeDObjectSystem();
	auto* Owner = Durin::NewObject<DTransactionRecordOwner>(nullptr, Durin::FName("TransactionRecordOwner"));
	Durin::TStrongObjectPtr<Durin::DObject> OwnerRoot(Owner);
	auto* Outer = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("TransactionRecordOuter"));
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(Outer, Durin::FName("TransactionRecordTarget"));
	auto* Sibling = Durin::NewObject<Durin::DObject>(Outer, Durin::FName("TransactionRecordSibling"));
	auto* Hard = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("TransactionRecordHard"));
	auto* Weak = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("TransactionRecordWeak"));
	const Durin::FObjectKey SiblingHandle = Durin::FObjectKey(Sibling);
	const Durin::FObjectKey WeakHandle = Durin::FObjectKey(Weak);
	Target->Hard = Hard;
	Target->Weak = Weak;
	auto HardRecord = CaptureRecord(Target);
	FCountingReferenceCollector CountingCollector;
	HardRecord.AddReferencedObjects(CountingCollector);
	EXPECT_TRUE(std::ranges::contains(CountingCollector.References, Target));
	EXPECT_TRUE(std::ranges::contains(CountingCollector.References, Hard));
	EXPECT_FALSE(std::ranges::contains(CountingCollector.References, Weak));
	Owner->Records.push_back(std::move(HardRecord));
	Owner->Records.push_back(Owner->Records.front());
	Owner->Records.erase(Owner->Records.begin());
	Owner->Records.push_back(CaptureRecord(Target));
	Target->Hard = nullptr;
	Target->Weak = nullptr;

	Durin::CollectGarbage();
	EXPECT_TRUE(Contains(Target));
	EXPECT_TRUE(Contains(Outer));
	EXPECT_TRUE(Contains(Hard));
	EXPECT_EQ(Durin::ResolveObjectKey(SiblingHandle), nullptr);
	EXPECT_EQ(Durin::ResolveObjectKey(WeakHandle), nullptr);

	const Durin::FObjectKey TargetHandle = Durin::FObjectKey(Target);
	const Durin::FObjectKey OuterHandle = Durin::FObjectKey(Outer);
	const Durin::FObjectKey HardHandle = Durin::FObjectKey(Hard);
	Owner->Records.clear();
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(TargetHandle), nullptr);
	EXPECT_EQ(Durin::ResolveObjectKey(OuterHandle), nullptr);
	EXPECT_EQ(Durin::ResolveObjectKey(HardHandle), nullptr);
}

TEST(FTransactionObjectRecordTests, MarkedGarbageTargetIsNotRescued)
{
	InitializeDObjectSystem();
	auto* Owner = Durin::NewObject<DTransactionRecordOwner>(nullptr, Durin::FName("GarbageRecordOwner"));
	Durin::TStrongObjectPtr<Durin::DObject> OwnerRoot(Owner);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, Durin::FName("GarbageRecordTarget"));
	Owner->Records.push_back(CaptureRecord(Target));
	const Durin::FObjectKey Handle = Durin::FObjectKey(Target);
	Durin::MarkAsGarbage(Target);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(Handle), nullptr);
	EXPECT_EQ(Owner->Records.front().GetTarget().Resolve(), nullptr);
}

TEST(FTransactionObjectRecordTests, RejectsIncompatibleReflectedSchemaBeforeMutation)
{
	InitializeDObjectSystem();
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "IncompatibleWholeRecord");
	Durin::TStrongObjectPtr<Durin::DObject> Root(Target);
	auto Record = CaptureRecord(Target);
	auto* Class = Target->GetClass();
	auto* OriginalProperties = Class->GetChildProperties();
	Durin::FNumericProperty Incompatible(Durin::FFieldVariant(Class), "Value", Durin::EObjectFlags::NoFlags,
		Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Value),
		sizeof(int64), Durin::DurinCodeGen::EPropertyGenFlags::Int64, nullptr);
	SetValueLifecycle<int64>(Incompatible);
	Incompatible.SetNext(OriginalProperties->GetNext());
	Class->SetChildProperties(&Incompatible);
	const auto Rejected = Record.Validate();
	Class->SetChildProperties(OriginalProperties);
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(Target->Value, 0);
	EXPECT_TRUE(Record.Validate());
}

TEST(FTransactionObjectRecordTests, AnnotationPrecedesSerializationAndPreflightDoesNotRecaptureIt)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationCaptureOwner"));
	std::vector<std::string> Trace; Target->CaptureTrace = &Trace;
	Target->bAnnotationEnabled = true; Target->SupplementalValue = 17;
	auto Record = FTransactionObjectRecord::CaptureWhole(*Target); ASSERT_TRUE(Record);
	ASSERT_EQ(Trace.size(), 2u); EXPECT_EQ(Trace[0], "annotation"); EXPECT_EQ(Trace[1], "serialize");
	ASSERT_TRUE(Record->Validate()); EXPECT_EQ(Target->AnnotationCaptures, 1u);
	Target->SupplementalValue = 29; ASSERT_TRUE(Record->CaptureAfter());
	EXPECT_FALSE(Record->IsNoOp()); ASSERT_TRUE(Record->Apply(true, EPropertyChangeOrigin::Undo));
	auto Annotation = std::dynamic_pointer_cast<FRecordTestAnnotation>(Record->GetReplayAnnotation());
	ASSERT_TRUE(Annotation); EXPECT_EQ(Annotation->Value, 17);
	Target->CaptureTrace = nullptr;
}

TEST(FTransactionObjectRecordTests, AnnotationAllocationOverflowRejectsAccounting)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationOverflowOwner"));
	Target->bAnnotationEnabled = true; Target->AnnotationBytes = std::numeric_limits<size_t>::max();
	auto Record = FTransactionObjectRecord::CaptureWhole(*Target); ASSERT_TRUE(Record);
	size_t Bytes = 0; EXPECT_FALSE(Record->TryGetAllocatedSize(Bytes));
}
