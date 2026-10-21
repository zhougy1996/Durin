#include "DObject/TransactionDiff.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/WeakObjectPtr.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	class FStateTestAnnotation final : public ITransactionObjectAnnotation
	{
	public:
		FStateTestAnnotation(int32 InValue, DObject* InHard, size_t InBytes) : Value(InValue), Hard(InHard), Bytes(InBytes) {}
		auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override { Hard.AddReferencedObjects(Collector); }
		auto Serialize(FArchive& Ar) -> void override { ++SerializeCalls; Ar << Value; }
		auto GetAllocatedSize() const -> size_t override { return Bytes; }
		int32 Value = 0;
		int32 SerializeCalls = 0;
		FPersistentObjectRef Hard;
		size_t Bytes = 0;
	};

	class DTransactionStateObject final : public DObject
	{
	public:
		explicit DTransactionStateObject(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : DObject(Initializer) {}
		static void __DefaultConstructor(const FObjectInitializer& Initializer) { new (Initializer.GetObj()) DTransactionStateObject(Initializer); }
		static auto StaticClass() -> DClass*
		{
			static DClass* Class = [] {
				auto* Result = new DClass(EC_StaticConstructor, "Tests::DTransactionStateObject",
					sizeof(DTransactionStateObject), alignof(DTransactionStateObject), EObjectFlags::NoFlags,
					EClassFlags::NoClassDefaultObject, EClassCastFlags::DClass,
					(DClass::ClassConstructorType)InternalConstructor<DTransactionStateObject>);
				Result->SetSuperStructBase(DObject::StaticClass());
				Result->SetTypeNames("DTransactionStateObject", "", "");
				auto Make = [&](const char* Name, size_t Offset, EPropertyFlags Flags) {
					auto* Field = new FNumericProperty(FFieldVariant(Result), Name, EObjectFlags::NoFlags,
						Flags, 1, static_cast<uint16>(Offset), sizeof(int32), DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
					const auto Ops = DurinCodeGen::MakePropertyValueOps<int32>();
					Field->SetValueLifecycle(Ops.ValueSize, Ops.ValueAlignment, Ops.InitializeValue, Ops.DestroyValue, Ops.CopyConstructValue, Ops.CopyAssignValue);
					return Field;
				};
				auto* Count = Make("Count", STRUCT_OFFSET_UINT16(DTransactionStateObject, Count), EPropertyFlags::Edit);
				auto* Session = Make("Session", STRUCT_OFFSET_UINT16(DTransactionStateObject, Session), EPropertyFlags::Transient);
				auto* Ignored = Make("Ignored", STRUCT_OFFSET_UINT16(DTransactionStateObject, Ignored), EPropertyFlags::NonTransactional);
				auto* Deprecated = Make("Deprecated", STRUCT_OFFSET_UINT16(DTransactionStateObject, Deprecated), EPropertyFlags::Deprecated);
				auto* Text = new FStringProperty(FFieldVariant(Result), "Text", EObjectFlags::NoFlags, EPropertyFlags::Edit,
					1, STRUCT_OFFSET_UINT16(DTransactionStateObject, Text), sizeof(std::string), DurinCodeGen::EPropertyGenFlags::String, nullptr);
				const auto TextOps = DurinCodeGen::MakePropertyValueOps<std::string>();
				Text->SetValueLifecycle(TextOps.ValueSize, TextOps.ValueAlignment, TextOps.InitializeValue, TextOps.DestroyValue, TextOps.CopyConstructValue, TextOps.CopyAssignValue);
				Count->SetNext(Session); Session->SetNext(Ignored); Ignored->SetNext(Deprecated); Deprecated->SetNext(Text); Result->SetChildProperties(Count);
				Result->Register(DClass::StaticClass, "", "DTransactionStateObject");
				return Result;
			}();
			return Class;
		}
		auto Serialize(FArchive& Ar) -> void override
		{
			if (CaptureTrace && Ar.IsSaving()) CaptureTrace->push_back("serialize");
			DObject::Serialize(Ar);
			{
				auto Type = FArchiveLogicalTypeDescriptor::Scalar(true, 32);
				Type.NativeFieldVersion = NativeVersion;
				auto Field = EnterArchiveField(Ar, {StaticClass()->GetQualifiedName(), "Native", Type});
				Ar << Native;
			}
			{
				auto Field = EnterArchiveField(Ar, {StaticClass()->GetQualifiedName(), "NativeHard", FArchiveLogicalTypeDescriptor::Object()});
				SerializeArchiveObjectReference(Ar, NativeHard);
			}
			{
				auto Field = EnterArchiveField(Ar, {StaticClass()->GetQualifiedName(), "NativeWeak", FArchiveLogicalTypeDescriptor::WeakObject()});
				SerializeArchiveWeakObjectReference(Ar, NativeWeak.GetBase());
			}
			Ar << RawNative;
			if (Ar.IsLoading() && bFailLoadOnce) { bFailLoadOnce = false; Ar.SetError("Injected object serialization load failure."); }
			if (Ar.IsLoading() && bThrowLoadOnce) { bThrowLoadOnce = false; throw std::runtime_error("Injected native serialization exception."); }
		}
		auto AddReferencedObjects(FReferenceCollector& Collector) -> void override
		{
			DObject::AddReferencedObjects(Collector);
			if (NativeHard) Collector.AddReferencedObject(NativeHard);
			if (ManagedState) ManagedState->AddReferencedObjects(Collector);
		}
		auto FindOrCreateTransactionAnnotation() const -> std::shared_ptr<ITransactionObjectAnnotation> override
		{
			if (!bEnableAnnotation) return {};
			++AnnotationCaptures;
			if (CaptureTrace) CaptureTrace->push_back("annotation");
			auto Result = std::make_shared<FStateTestAnnotation>(AnnotationValue, AnnotationHard, AnnotationBytes);
			if (bCollectDuringAnnotation) CollectGarbage();
			return Result;
		}
		int32 Count = 0, Session = 0, Ignored = 0, Native = 0, Deprecated = 0;
		int32 RawNative = 0;
		std::string Text;
		DObject* NativeHard = nullptr;
		TWeakObjectPtr<DObject> NativeWeak;
		bool bFailLoadOnce = false, bThrowLoadOnce = false;
		uint32 NativeVersion = 1;
		bool bEnableAnnotation = false, bCollectDuringAnnotation = false;
		mutable uint32 AnnotationCaptures = 0;
		int32 AnnotationValue = 0;
		DObject* AnnotationHard = nullptr;
		size_t AnnotationBytes = sizeof(FStateTestAnnotation);
		std::vector<std::string>* CaptureTrace = nullptr;
		std::optional<FSerializedObjectCapture> ManagedState;
	};
}

TEST(FTransactionSerializationTests, NativeTransferDoesNotAcquireAnnotations)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "PayloadWithoutAnnotations"));
	Object->bEnableAnnotation = true;
	Object->Count = 11; Object->Session = 12; Object->Native = 13; Object->Ignored = 14;
	auto Captured = CaptureSerializedObject(*Object); ASSERT_TRUE(Captured);
	EXPECT_EQ(Object->AnnotationCaptures, 0u);
	Object->Count = 21; Object->Session = 22; Object->Native = 23; Object->Ignored = 24;
	ASSERT_TRUE(ValidateSerializedObject(*Object, *Captured));
	ASSERT_TRUE(LoadSerializedObject(*Object, *Captured));
	EXPECT_EQ(Object->Count, 11); EXPECT_EQ(Object->Session, 12); EXPECT_EQ(Object->Native, 13);
	EXPECT_EQ(Object->Ignored, 24);
	EXPECT_EQ(Object->AnnotationCaptures, 0u);
}

TEST(FTransactionSerializationTests, TransferFailureLeavesCompensationToReplayOwner)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "PayloadFailureOwner"));
	Object->Count = 31;
	auto Captured = CaptureSerializedObject(*Object); ASSERT_TRUE(Captured);
	Object->Count = 41;
	ASSERT_TRUE(ValidateSerializedObject(*Object, *Captured));
	Object->bFailLoadOnce = true;
	auto Loaded = LoadSerializedObject(*Object, *Captured);
	ASSERT_FALSE(Loaded); EXPECT_FALSE(Loaded.error().bRecoveryRequired);
	// Transfer has written the requested value; it does not capture or apply an
	// inverse. The coordinator's existing failure contracts test compensation.
	EXPECT_EQ(Object->Count, 31);
}

TEST(FTransactionSerializationTests, VirtualSerializationIncludesNativeAndTransientButNotExcludedFields)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "NativeStateValues"));
	Object->Count = 3; Object->Session = 4; Object->Ignored = 5; Object->Native = 6;
	FDiffableObject Observation;
	auto Before = CaptureSerializedObject(*Object, nullptr, &Observation);
	ASSERT_TRUE(Before) << Before.error();
	Object->Count = 30; Object->Session = 40; Object->Ignored = 50; Object->Native = 60; Object->Deprecated = 70;
	ASSERT_TRUE(ValidateSerializedObject(*Object, *Before));
	ASSERT_TRUE(LoadSerializedObject(*Object, *Before));
	EXPECT_EQ(Object->Count, 3); EXPECT_EQ(Object->Session, 4); EXPECT_EQ(Object->Ignored, 50); EXPECT_EQ(Object->Native, 6);
	EXPECT_EQ(Object->Deprecated, 70);
	EXPECT_TRUE(std::ranges::any_of(Observation.SerializedTaggedData, [](const auto& Field) { return Field.Name == FName("Native") && !Field.bReflected; }));
	EXPECT_TRUE(std::ranges::any_of(Observation.SerializedTaggedData, [](const auto& Field) { return Field.Name == FName("Count") && Field.bReflected; }));
	size_t Bytes = 0;
	EXPECT_TRUE(Before->TryGetAllocatedSize(Bytes)); EXPECT_GT(Bytes, 0u);
}

TEST(FTransactionSerializationTests, RestoresExactNameOuterAndMutableFlagsWithoutResurrection)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DObject> FirstOuter(NewObject<DObject>(nullptr, "StateFirstOuter"));
	TStrongObjectPtr<DObject> SecondOuter(NewObject<DObject>(nullptr, "StateSecondOuter"));
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(FirstOuter.Get(), "Original"));
	const auto Key = FObjectKey(Object.Get());
	auto Before = CaptureSerializedObject(*Object); ASSERT_TRUE(Before);
	Object->Rename("Edited"); Object->SetOuterPrivate(SecondOuter.Get()); Object->SetFlags(EObjectFlags::Public | EObjectFlags::Transient);
	ASSERT_TRUE(ValidateSerializedObject(*Object, *Before));
	ASSERT_TRUE(LoadSerializedObject(*Object, *Before));
	EXPECT_EQ(Object->GetFName(), FName("Original")); EXPECT_EQ(Object->GetOuter(), FirstOuter.Get());
	EXPECT_FALSE(Object->HasAnyObjectFlags(EObjectFlags::Public | EObjectFlags::Transient));
	EXPECT_EQ(FObjectKey(Object.Get()), Key);
	auto* GarbageObject = Object.Get();
	MarkAsGarbage(GarbageObject);
	EXPECT_FALSE(ValidateSerializedObject(*GarbageObject, *Before));
	EXPECT_TRUE(GarbageObject->IsGarbage());
}

TEST(FTransactionSerializationTests, NameConflictRejectsBeforeAnySerializedFieldMutation)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DObject> Outer(NewObject<DObject>(nullptr, "StateCollisionOuter"));
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(Outer.Get(), "Reserved"));
	auto Before = CaptureSerializedObject(*Object); ASSERT_TRUE(Before);
	Object->Rename("Moved"); Object->Count = 7;
	TStrongObjectPtr<DObject> Impostor(NewObject<DObject>(Outer.Get(), "Reserved"));
	EXPECT_FALSE(ValidateSerializedObject(*Object, *Before));
	EXPECT_EQ(Object->Count, 7); EXPECT_EQ(Object->GetFName(), FName("Moved"));
}

TEST(FTransactionSerializationTests, CollectorOwnedStateRetainsHardReferencesButNotWeakReferences)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "StateReferenceTarget"));
	TStrongObjectPtr<DTransactionStateObject> Owner(NewObject<DTransactionStateObject>(nullptr, "StateReferenceOwner"));
	Object->NativeHard = NewObject<DObject>(nullptr, "StateHard");
	auto* Weak = NewObject<DObject>(nullptr, "StateWeak"); Object->NativeWeak = Weak;
	const auto HardKey = FObjectKey(Object->NativeHard); const auto WeakKey = FObjectKey(Weak);
	auto State = CaptureSerializedObject(*Object); ASSERT_TRUE(State);
	Owner->ManagedState = std::move(*State); Object->NativeHard = nullptr; Object->NativeWeak = nullptr;
	CollectGarbage();
	EXPECT_NE(ResolveObjectKey(HardKey), nullptr); EXPECT_EQ(ResolveObjectKey(WeakKey), nullptr);
	ASSERT_TRUE(ValidateSerializedObject(*Object, *Owner->ManagedState));
	ASSERT_TRUE(LoadSerializedObject(*Object, *Owner->ManagedState));
	EXPECT_EQ(Object->NativeHard, ResolveObjectKey(HardKey)); EXPECT_FALSE(Object->NativeWeak.IsValid());
	Object->NativeHard = nullptr; Owner->ManagedState.reset(); CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(HardKey), nullptr);
}

TEST(FTransactionSerializationTests, NativeExceptionReportsRecoveryWithoutImplicitCompensation)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "TransferNativeException"));
	auto Capture = CaptureSerializedObject(*Object); ASSERT_TRUE(Capture);
	Object->Count = 17; Object->Native = 27;
	ASSERT_TRUE(ValidateSerializedObject(*Object, *Capture));
	Object->bThrowLoadOnce = true;
	auto Thrown = LoadSerializedObject(*Object, *Capture); ASSERT_FALSE(Thrown);
	EXPECT_TRUE(Thrown.error().bRecoveryRequired);
	EXPECT_EQ(Object->Count, 0); EXPECT_EQ(Object->Native, 0);
}

TEST(FTransactionSerializationTests, IncompatibleNativeSchemaAndExpiredHardReferenceRejectBeforeLoad)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "StateSchemaExpiry"));
	TStrongObjectPtr<DObject> Reference(NewObject<DObject>(nullptr, "StateExpiringHardReference"));
	Object->NativeHard = Reference.Get();
	auto State = CaptureSerializedObject(*Object); ASSERT_TRUE(State);
	Object->Count = 8; Object->Native = 9; Object->NativeVersion = 2;
	EXPECT_FALSE(ValidateSerializedObject(*Object, *State)); EXPECT_EQ(Object->Count, 8); EXPECT_EQ(Object->Native, 9);
	Object->NativeVersion = 1;
	MarkAsGarbage(Reference.Get()); Object->NativeHard = nullptr;
	EXPECT_FALSE(ValidateSerializedObject(*Object, *State)); EXPECT_EQ(Object->Count, 8); EXPECT_EQ(Object->Native, 9);
}

TEST(FTransactionDiffTests, DiffReportsActualFieldsNativeDataAndOriginalIdentity)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DObject> Outer(NewObject<DObject>(nullptr, "DiffOriginalOuter"));
	TStrongObjectPtr<DObject> NextOuter(NewObject<DObject>(nullptr, "DiffNextOuter"));
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(Outer.Get(), "DiffOriginalName"));
	const auto OriginalPath = Outer->GetObjectPath();
	auto Before = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(Before);
	Object->Count = 3; Object->Session = 4; Object->Ignored = 5; Object->Native = 7;
	Object->Rename("DiffNewName"); Object->SetOuterPrivate(NextOuter.Get());
	auto After = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(After);
	auto Changes = DiffUtil::GenerateObjectDiff(*Before, *After); ASSERT_TRUE(Changes);
	EXPECT_TRUE(Changes->HasChanges()); EXPECT_TRUE(Changes->bNameChanged); EXPECT_TRUE(Changes->bOuterChanged); EXPECT_TRUE(Changes->bNonPropertyDataChanged);
	EXPECT_EQ(Changes->OriginalObject, FObjectKey(Object.Get())); EXPECT_EQ(Changes->OriginalName, FName("DiffOriginalName"));
	EXPECT_EQ(Changes->OriginalOuterPath, OriginalPath); EXPECT_EQ(Changes->OriginalClassPath, Object->GetClass()->GetQualifiedName().ToString());
	EXPECT_TRUE(std::ranges::contains(Changes->ChangedProperties, FName("Count")));
	EXPECT_TRUE(std::ranges::contains(Changes->ChangedProperties, FName("Session")));
	EXPECT_FALSE(std::ranges::contains(Changes->ChangedProperties, FName("Ignored")));
	EXPECT_FALSE(std::ranges::contains(Changes->ChangedProperties, FName("Native")));
}

TEST(FTransactionDiffTests, VariableReflectedRangesDoNotInventNativeChanges)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "DiffVariableRanges"));
	auto Before = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(Before);
	Object->Text = std::string(200, 'x');
	auto After = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(After);
	auto Changes = DiffUtil::GenerateObjectDiff(*Before, *After); ASSERT_TRUE(Changes);
	EXPECT_EQ(Changes->ChangedProperties, (std::vector<FName>{"Text"})); EXPECT_FALSE(Changes->bNonPropertyDataChanged);
	Object->RawNative = 8;
	auto WithRaw = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(WithRaw);
	auto Native = DiffUtil::GenerateObjectDiff(*After, *WithRaw); ASSERT_TRUE(Native);
	EXPECT_TRUE(Native->bNonPropertyDataChanged); EXPECT_TRUE(Native->ChangedProperties.empty());
}

TEST(FTransactionDiffTests, PropertiesOnlyObservationBypassesNativeSerialization)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "PropertiesOnlyDiff"));
	std::vector<std::string> Trace; Object->CaptureTrace = &Trace;
	const DiffUtil::FGetDiffableObjectOptions Options{DiffUtil::EGetDiffableObjectMode::SerializeProperties};
	auto Before = DiffUtil::GetDiffableObject(*Object, Options); ASSERT_TRUE(Before);
	Object->Count = 9; Object->RawNative = 8;
	auto After = DiffUtil::GetDiffableObject(*Object, Options); ASSERT_TRUE(After);
	auto Changes = DiffUtil::GenerateObjectDiff(*Before, *After); ASSERT_TRUE(Changes);
	EXPECT_EQ(Changes->ChangedProperties, std::vector<FName>{"Count"}); EXPECT_FALSE(Changes->bNonPropertyDataChanged);
	EXPECT_TRUE(Trace.empty()); EXPECT_TRUE(CaptureSerializedObject(*Object));
	EXPECT_EQ(Trace, std::vector<std::string>{"serialize"}); Object->CaptureTrace = nullptr;
}


TEST(FTransactionDiffTests, DiffRejectsIncompatibleTargetsAndNativeSchemas)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "DiffCompatible"));
	TStrongObjectPtr<DTransactionStateObject> Other(NewObject<DTransactionStateObject>(nullptr, "DiffDifferentTarget"));
	auto Before = DiffUtil::GetDiffableObject(*Object); auto Different = DiffUtil::GetDiffableObject(*Other);
	ASSERT_TRUE(Before); ASSERT_TRUE(Different); EXPECT_FALSE(DiffUtil::GenerateObjectDiff(*Before, *Different));
	Object->NativeVersion = 2;
	auto Changed = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(Changed); EXPECT_FALSE(DiffUtil::GenerateObjectDiff(*Before, *Changed));
}

TEST(FTransactionDiffTests, SharedObservationUsesTransferOnceWithoutAcquiringAnnotations)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "SharedObservation"));
	Object->bEnableAnnotation = true;
	FDiffableObject Observation;
	auto Capture = CaptureSerializedObject(*Object, nullptr, &Observation); ASSERT_TRUE(Capture);
	EXPECT_EQ(Capture->Data, Observation.SerializedObject);
	EXPECT_EQ(Capture->Context, Observation.SerializationContext);
	EXPECT_EQ(Object->AnnotationCaptures, 0u);
	Object->Count = 7;
	auto Current = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(Current);
	auto Changes = DiffUtil::GenerateObjectDiff(Observation, *Current); ASSERT_TRUE(Changes);
	EXPECT_EQ(Changes->ChangedProperties, (std::vector<FName>{"Count"}));
	size_t CaptureBytes = 0, ObservationBytes = 0, ReservedBytes = 0;
	ASSERT_TRUE(Capture->TryGetAllocatedSize(CaptureBytes));
	ASSERT_TRUE(Observation.TryGetAllocatedSize(ObservationBytes));
	EXPECT_EQ(ObservationBytes, CaptureBytes + Observation.SerializedTaggedData.capacity() * sizeof(FSerializedTaggedData));
	auto& OuterPath = Observation.ObjectInfo.Identity.OriginalOuterPath;
	const auto OriginalCapacity = OuterPath.capacity();
	OuterPath.reserve(4096);
	ASSERT_TRUE(Observation.TryGetAllocatedSize(ReservedBytes));
	EXPECT_EQ(ReservedBytes, ObservationBytes + OuterPath.capacity() - OriginalCapacity);
}

TEST(FTransactionDiffTests, LargeObservationsShareTransferStorageAndReleasePreviousSnapshots)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "LargeSharedObservation"));
	Object->Text.assign(1024u * 1024u, 'x');
	std::vector<std::string> Trace; Object->CaptureTrace = &Trace;
	Object->bEnableAnnotation = true;
	FDiffableObject Observation;
	std::weak_ptr<const FSerializedObject> PreviousPayload;
	std::weak_ptr<const FTransactionSerializationContext> PreviousContext;
	size_t RetainedBytes = 0, PayloadBytes = 0;
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		Object->Count = static_cast<int32>(Index);
		auto Capture = CaptureSerializedObject(*Object, nullptr, &Observation); ASSERT_TRUE(Capture);
		EXPECT_TRUE(PreviousPayload.expired()); EXPECT_TRUE(PreviousContext.expired());
		EXPECT_EQ(Capture->Data, Observation.SerializedObject);
		EXPECT_EQ(Capture->Context, Observation.SerializationContext);
		EXPECT_EQ(Capture->Data.use_count(), 2); EXPECT_EQ(Capture->Context.use_count(), 2);
		EXPECT_EQ(Trace.size(), Index + 1u); EXPECT_EQ(Object->AnnotationCaptures, 0u);
		PayloadBytes = Capture->Data->Bytes.size();
		EXPECT_GE(PayloadBytes, Object->Text.size()); EXPECT_LT(PayloadBytes, Object->Text.size() + 4096u);
		size_t CurrentBytes = 0; ASSERT_TRUE(Observation.TryGetAllocatedSize(CurrentBytes));
		if (Index == 0) RetainedBytes = CurrentBytes;
		else EXPECT_EQ(CurrentBytes, RetainedBytes);
		PreviousPayload = Capture->Data; PreviousContext = Capture->Context;
		*Capture = {};
		EXPECT_FALSE(PreviousPayload.expired()); EXPECT_FALSE(PreviousContext.expired());
		EXPECT_EQ(Observation.SerializedObject.use_count(), 1);
	}
	Observation = {};
	EXPECT_TRUE(PreviousPayload.expired()); EXPECT_TRUE(PreviousContext.expired());
	Object->CaptureTrace = nullptr;
	RecordProperty("payload_bytes", std::to_string(PayloadBytes));
	RecordProperty("retained_observation_bytes", std::to_string(RetainedBytes));
	RecordProperty("captures", "32");
}

TEST(FTransactionDiffTests, PartialDiffSkipsMetadataAndFiltersOnlyReflectedProperties)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "PartialObservation"));
	auto Before = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(Before);
	Object->Rename("PartialObservationRenamed"); Object->Count = 3; Object->Session = 4; Object->RawNative = 9;
	auto After = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(After);
	auto Changes = DiffUtil::GenerateObjectDiff(*Before, *After, {.bFullDiff = false,
		.ShouldSkipProperty = [](FName Name) { return Name != FName("Session"); }}); ASSERT_TRUE(Changes);
	EXPECT_FALSE(Changes->bNameChanged); EXPECT_TRUE(Changes->bNonPropertyDataChanged);
	EXPECT_EQ(Changes->ChangedProperties, (std::vector<FName>{"Session"}));
}

TEST(FTransactionDiffTests, InvalidAndOverlappingTaggedRangesRejectComparison)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DTransactionStateObject> Object(NewObject<DTransactionStateObject>(nullptr, "InvalidObservation"));
	auto Before = DiffUtil::GetDiffableObject(*Object); ASSERT_TRUE(Before);
	auto After = *Before;
	ASSERT_GE(After.SerializedTaggedData.size(), 2u);
	After.SerializedTaggedData[1].Begin = 0;
	EXPECT_FALSE(DiffUtil::GenerateObjectDiff(*Before, After));
	After = *Before; After.SerializedTaggedData[0].End = After.SerializedObject->Bytes.size() + 1;
	EXPECT_FALSE(DiffUtil::GenerateObjectDiff(*Before, After));
}
