#include "Transactions/TransactionRecord.h"
#include "Transactions/Transactor.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "EngineTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace Durin
{
	struct FTransBufferTestAccess
	{
		static auto InstallHistory(DTransBuffer& Buffer, Editor::FTransaction Transaction) -> void
		{
			Buffer.History.push_back(std::move(Transaction));
			Buffer.Cursor = 1;
		}
		static auto SetState(DTransBuffer& Buffer, Editor::ETransactorState State) -> void
		{
			Buffer.State = State;
		}
	};
}

namespace
{
	template<typename T>
	auto SetValueLifecycle(Durin::FProperty& Property) -> void
	{
		const auto Ops = Durin::DurinCodeGen::MakePropertyValueOps<T>();
		Property.SetValueLifecycle(
			Ops.ValueSize, Ops.ValueAlignment, Ops.InitializeValue, Ops.DestroyValue,
			Ops.CopyConstructValue, Ops.CopyAssignValue);
	}

	struct FRecordNestedValue
	{
		int32 Count = 0;
	};

	using FRecordMap = std::unordered_map<std::string, int32>;

	class DTransactionRecordParticipant final : public Durin::DObject
	{
	public:
		explicit DTransactionRecordParticipant(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DTransactionRecordParticipant(X);
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (Class) return Class;
			Class = new Durin::DClass(
				Durin::EC_StaticConstructor, Durin::FName("Tests::DTransactionRecordParticipant"),
				sizeof(DTransactionRecordParticipant), alignof(DTransactionRecordParticipant),
				Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
				Durin::EClassCastFlags::DClass,
				(Durin::DClass::ClassConstructorType)
					Durin::InternalConstructor<DTransactionRecordParticipant>);
			Class->SetSuperStructBase(Durin::DObject::StaticClass());
			Class->SetTypeNames("DTransactionRecordParticipant", "", "");

			auto* ValueProperty = new Durin::FNumericProperty(
				Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Value),
				sizeof(int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetValueLifecycle<int32>(*ValueProperty);

			auto* HardProperty = new Durin::FObjectProperty(
				Durin::FFieldVariant(Class), Durin::FName("Hard"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Hard),
				sizeof(Durin::TObjectPtr<Durin::DObject>),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(), true,
				[](const void* Value) -> Durin::DObject* {
					return static_cast<const Durin::TObjectPtr<Durin::DObject>*>(Value)->Get();
				},
				[](void* Value, Durin::DObject* Object) {
					*static_cast<Durin::TObjectPtr<Durin::DObject>*>(Value) = Object;
				});
			SetValueLifecycle<Durin::TObjectPtr<Durin::DObject>>(*HardProperty);

			auto* WeakProperty = new Durin::FWeakObjectProperty(
				Durin::FFieldVariant(Class), Durin::FName("Weak"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit | Durin::EPropertyFlags::Transient, 1,
				STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Weak),
				sizeof(Durin::TWeakObjectPtr<Durin::DObject>), Durin::DObject::StaticClass(),
				[](void* Value) -> Durin::FWeakObjectPtr* {
					return &static_cast<Durin::TWeakObjectPtr<Durin::DObject>*>(Value)->GetBase();
				},
				[](const void* Value) -> const Durin::FWeakObjectPtr* {
					return &static_cast<const Durin::TWeakObjectPtr<Durin::DObject>*>(Value)->GetBase();
				});
			SetValueLifecycle<Durin::TWeakObjectPtr<Durin::DObject>>(*WeakProperty);

			auto* SoftProperty = new Durin::FSoftObjectProperty(
				Durin::FFieldVariant(Class), Durin::FName("Soft"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Soft),
				sizeof(Durin::TSoftObjectPtr<Durin::DObject>), Durin::DObject::StaticClass(),
				[](void* Value) -> Durin::FSoftObjectPtr* {
					return &static_cast<Durin::TSoftObjectPtr<Durin::DObject>*>(Value)->GetBase();
				},
				[](const void* Value) -> const Durin::FSoftObjectPtr* {
					return &static_cast<const Durin::TSoftObjectPtr<Durin::DObject>*>(Value)->GetBase();
				});
			SetValueLifecycle<Durin::TSoftObjectPtr<Durin::DObject>>(*SoftProperty);

			auto* ArrayInner = new Durin::FNumericProperty(
				Durin::FFieldVariant(), Durin::FName("Numbers_Inner"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::None, 1, 0, sizeof(int32),
				Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetValueLifecycle<int32>(*ArrayInner);
			auto* ArrayProperty = new Durin::FArrayProperty(
				Durin::FFieldVariant(Class), Durin::FName("Numbers"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Numbers),
				sizeof(std::vector<int32>), Durin::DurinCodeGen::EPropertyGenFlags::Array,
				nullptr, Durin::ResolveArrayOps<std::vector<int32>>());
			ArrayProperty->SetInner(ArrayInner);
			SetValueLifecycle<std::vector<int32>>(*ArrayProperty);

			auto* MapKey = new Durin::FStringProperty(
				Durin::FFieldVariant(), Durin::FName("Values_Key"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::None, 1, 0, sizeof(std::string),
				Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr);
			SetValueLifecycle<std::string>(*MapKey);
			auto* MapValue = new Durin::FNumericProperty(
				Durin::FFieldVariant(), Durin::FName("Values_Value"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::None, 1, 0, sizeof(int32),
				Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetValueLifecycle<int32>(*MapValue);
			auto* MapProperty = new Durin::FMapProperty(
				Durin::FFieldVariant(Class), Durin::FName("Values"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Values),
				sizeof(FRecordMap), Durin::DurinCodeGen::EPropertyGenFlags::Map,
				nullptr, Durin::ResolveMapOps<FRecordMap>());
			MapProperty->SetKeyProp(MapKey);
			MapProperty->SetValueProp(MapValue);
			SetValueLifecycle<FRecordMap>(*MapProperty);

			auto* NestedStruct = new Durin::DStruct(
				Durin::EC_StaticConstructor, Durin::FName("Tests::FRecordNestedValue"),
				Durin::FName("FRecordNestedValue"), sizeof(FRecordNestedValue),
				alignof(FRecordNestedValue), Durin::EObjectFlags::Transient);
			NestedStruct->InitializeOps(&Durin::GetDStructOps<FRecordNestedValue>());
			auto* NestedCount = new Durin::FNumericProperty(
				Durin::FFieldVariant(NestedStruct), Durin::FName("Count"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::None, 1, offsetof(FRecordNestedValue, Count), sizeof(int32),
				Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetValueLifecycle<int32>(*NestedCount);
			NestedStruct->ChildProperties = NestedCount;
			auto* NestedProperty = new Durin::FStructProperty(
				Durin::FFieldVariant(Class), Durin::FName("Nested"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Nested),
				NestedStruct);

			ValueProperty->Next = HardProperty;
			HardProperty->Next = WeakProperty;
			WeakProperty->Next = SoftProperty;
			SoftProperty->Next = ArrayProperty;
			ArrayProperty->Next = MapProperty;
			MapProperty->Next = NestedProperty;
			Class->ChildProperties = ValueProperty;
			Class->Register(Durin::DClass::StaticClass, "", "DTransactionRecordParticipant");
			Durin::DObjectForceRegistration(Class);
			return Class;
		}

		int32 Value = 0;
		Durin::TObjectPtr<Durin::DObject> Hard;
		Durin::TWeakObjectPtr<Durin::DObject> Weak;
		Durin::TSoftObjectPtr<Durin::DObject> Soft;
		std::vector<int32> Numbers;
		FRecordMap Values;
		FRecordNestedValue Nested;
	};

	class DTransactionRecordOwner final : public Durin::DObject
	{
	public:
		explicit DTransactionRecordOwner(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
		}

		static void __DefaultConstructor(const Durin::FObjectInitializer& X)
		{
			new (X.GetObj()) DTransactionRecordOwner(X);
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (!Class)
			{
				Class = new Durin::DClass(
					Durin::EC_StaticConstructor, Durin::FName("Tests::DTransactionRecordOwner"),
					sizeof(DTransactionRecordOwner), alignof(DTransactionRecordOwner),
					Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
					Durin::EClassCastFlags::DClass,
					(Durin::DClass::ClassConstructorType)
						Durin::InternalConstructor<DTransactionRecordOwner>);
				Class->SetSuperStructBase(Durin::DObject::StaticClass());
				Class->SetTypeNames("DTransactionRecordOwner", "", "");
				Class->Register(Durin::DClass::StaticClass, "", "DTransactionRecordOwner");
				Durin::DObjectForceRegistration(Class);
			}
			return Class;
		}

		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) -> void override
		{
			DObject::AddReferencedObjects(Collector);
			for (const Durin::Editor::FFocusedTransactionObjectSnapshot& Snapshot : Records)
				Snapshot.AddReferencedObjects(Collector);
		}

		std::vector<Durin::Editor::FFocusedTransactionObjectSnapshot> Records;
	};

	class FCountingReferenceCollector final : public Durin::FReferenceCollector
	{
	public:
		auto AddReferencedObject(Durin::DObject*& Object) -> void override
		{
			References.push_back(Object);
		}

		std::vector<Durin::DObject*> References;
	};

	class FTestCustomChange final : public Durin::Editor::ITransactionCustomChange
	{
	public:
		FTestCustomChange(int& InValue, int InBefore, int InAfter,
			Durin::DObject* InReference = nullptr,
			std::string InModule = {})
			: Value(InValue), Before(InBefore), After(InAfter), Reference(InReference),
			  Module(std::move(InModule))
		{
		}

		auto GetDescription() const -> std::string_view override { return "Test custom change"; }
		auto GetDetails(Durin::Editor::ETransactionOperation) const -> std::string override
		{
			return Details;
		}
		auto Replay(Durin::Editor::ETransactionOperation Operation) -> Durin::Editor::FTransactionCustomResult override
		{
			if (Operation == Durin::Editor::ETransactionOperation::Undo)
			{
				if (bFailUndo) { return {{.Code = Durin::Editor::ETransactionCustomError::InjectedMutation}}; }
				if (bDeferUndo) { bPending = true; return {}; }
				Value = Before;
				return {};
			}
			else
			{
				if (bFailRedo)
				{
					return {{.Code = Durin::Editor::ETransactionCustomError::InjectedMutation}};
				}
				Value = After;
				return {};
			}
		}
		auto IsDeferredOperationPending() const -> bool override { return bPending; }
		auto SetDeferredOperationCompletion(
			Durin::Editor::FTransactionDeferredCompletion InCompletion) -> void override
		{
			Completion = std::move(InCompletion);
		}
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) const -> void override
		{
			Durin::DObject* Object = Reference.Get();
			if (Object) Collector.AddReferencedObject(Object);
		}
		auto GetAllocatedSize() const -> size_t override
		{
			return Details.capacity() + Module.capacity();
		}
		auto GetOwningModule() const -> std::string_view override { return Module; }

		auto CompleteUndo(Durin::Editor::FTransactionCompletionResult Result) -> void
		{
			if (!bPending) return;
			bPending = false;
			if (Result) Value = Before;
			auto Callback = Completion;
			if (Callback) Callback(std::move(Result));
		}

		int& Value;
		int Before = 0;
		int After = 0;
		Durin::TObjectPtr<Durin::DObject> Reference;
		std::string Module;
		std::string Details;
		Durin::Editor::FTransactionDeferredCompletion Completion;
		bool bFailUndo = false;
		bool bFailRedo = false;
		bool bDeferUndo = false;
		bool bPending = false;
	};

	// Qualifies expression-style deletion: history owns the detached child, not a garbage object.
	class FDetachedChildChange final : public Durin::Editor::ITransactionCustomChange
	{
	public:
		FDetachedChildChange(DTransactionRecordParticipant* InOwner, Durin::DObject* InChild,
			Durin::DObject* InHistoryOuter)
			: Owner(InOwner), Child(InChild), HistoryOuter(InHistoryOuter) {}

		auto GetDescription() const -> std::string_view override { return "Detach owned child"; }
		auto Replay(Durin::Editor::ETransactionOperation Operation) -> Durin::Editor::FTransactionCustomResult override
		{
			if (Operation == Durin::Editor::ETransactionOperation::Undo)
			{
				Child->SetOuterPrivate(Owner);
				Owner->Hard = Child;
				return {};
			}
			else
			{
				Owner->Hard = nullptr;
				Child->SetOuterPrivate(HistoryOuter);
				return {};
			}
		}
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) const -> void override
		{
			for (Durin::DObject* Object : {static_cast<Durin::DObject*>(Owner), Child, HistoryOuter})
				Collector.AddReferencedObject(Object);
		}
		auto GetAllocatedSize() const -> size_t override { return 0; }

	private:
		DTransactionRecordParticipant* Owner;
		Durin::DObject* Child;
		Durin::DObject* HistoryOuter;
	};

	auto Property(std::string_view Name) -> Durin::FProperty*
	{
		return DTransactionRecordParticipant::StaticClass()->FindPropertyByName(Durin::FName(Name));
	}

	auto Contains(const Durin::DObject* Object) -> bool
	{
		return Durin::GDObjectArray.Contains(Object);
	}

	auto MakeSoftPath() -> Durin::FObjectPath
	{
		static const bool bMounted = [] {
			const std::filesystem::path Root =
				Durin::Testing::GetTestWorkDirectory() / "TransactionRecordSoftAssets";
			std::filesystem::create_directories(Root);
			Durin::Testing::RegisterMountPointForTests(
				"/TransactionTests/", Root.generic_string() + "/");
			return true;
		}();
		(void)bMounted;
		Durin::FObjectPath Path;
		EXPECT_TRUE(Durin::FObjectPath::TryCreate(
			"/TransactionTests/SoftAsset.SoftAsset", Path));
		return Path;
	}

	auto CaptureSnapshot(
		DTransactionRecordParticipant* Target,
		std::string_view Name) -> Durin::Editor::FFocusedTransactionObjectSnapshot
	{
		auto Result = Durin::Editor::FFocusedTransactionObjectSnapshot::Capture(
			Target, Property(Name), 0);
		EXPECT_TRUE(Result) << Durin::Editor::FormatTransactionSnapshotError(Result.error());
		return Result ? std::move(*Result) : Durin::Editor::FFocusedTransactionObjectSnapshot{};
	}
}

TEST(FPersistentObjectRefTests, PreservesExactIdentityAndRejectsGarbageAndSlotReuse)
{
	InitializeDObjectSystem();
	auto* FirstOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PersistentFirstOuter"));
	auto* SecondOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PersistentSecondOuter"));
	auto* Object = Durin::NewObject<Durin::DObject>(FirstOuter, Durin::FName("PersistentObject"));
	const Durin::Editor::FPersistentObjectRef Reference(Object);
	EXPECT_EQ(Durin::Editor::FPersistentObjectRef(), Durin::Editor::FPersistentObjectRef(nullptr));
	EXPECT_FALSE(Reference.IsNull());
	EXPECT_FALSE(Reference.IsStale());
	EXPECT_EQ(Reference.Resolve(), Object);

	Object->Rename(Durin::FName("PersistentObjectRenamed"));
	Object->SetOuterPrivate(SecondOuter);
	EXPECT_EQ(Reference.Resolve(), Object);
	Durin::MarkAsGarbage(Object);
	EXPECT_EQ(Reference.Resolve(), nullptr);
	EXPECT_TRUE(Reference.IsStale());
	Durin::CollectGarbage();
	EXPECT_EQ(Reference.Resolve(), nullptr);

	auto* Reused = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PersistentReusedSlot"));
	EXPECT_NE(Durin::FObjectKey(Reused), Reference.GetKey());
	EXPECT_EQ(Reference.Resolve(), nullptr);
	Durin::MarkAsGarbage(Reused);
	Durin::CollectGarbage();
}

TEST(FFocusedTransactionObjectSnapshotTests, CollectorTraversalRetainsOnlyTargetAndHardReferences)
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
	auto HardRecord = CaptureSnapshot(Target, "Hard");
	FCountingReferenceCollector CountingCollector;
	HardRecord.AddReferencedObjects(CountingCollector);
	ASSERT_EQ(CountingCollector.References.size(), 2u);
	EXPECT_EQ(CountingCollector.References[0], Target);
	EXPECT_EQ(CountingCollector.References[1], Hard);
	Owner->Records.push_back(std::move(HardRecord));
	Owner->Records.push_back(Owner->Records.front());
	Owner->Records.erase(Owner->Records.begin());
	Owner->Records.push_back(CaptureSnapshot(Target, "Weak"));
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

TEST(FFocusedTransactionObjectSnapshotTests, MarkedGarbageTargetIsNotRescued)
{
	InitializeDObjectSystem();
	auto* Owner = Durin::NewObject<DTransactionRecordOwner>(nullptr, Durin::FName("GarbageRecordOwner"));
	Durin::TStrongObjectPtr<Durin::DObject> OwnerRoot(Owner);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, Durin::FName("GarbageRecordTarget"));
	Owner->Records.push_back(CaptureSnapshot(Target, "Value"));
	const Durin::FObjectKey Handle = Durin::FObjectKey(Target);
	Durin::MarkAsGarbage(Target);
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(Handle), nullptr);
	EXPECT_EQ(Owner->Records.front().GetTarget().Resolve(), nullptr);
}

TEST(FFocusedTransactionObjectSnapshotTests, RestoresSupportedValuesIntoDetachedStorage)
{
	InitializeDObjectSystem();
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, Durin::FName("DetachedRestoreTarget"));
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	auto* Hard = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("DetachedRestoreHard"));
	auto* Weak = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("DetachedRestoreWeak"));
	Target->Value = 47;
	Target->Hard = Hard;
	Target->Weak = Weak;
	const Durin::FObjectPath SoftPath = MakeSoftPath();
	Target->Soft.SetPath(SoftPath);
	Target->Numbers = {3, 5, 8};
	Target->Values = {{"alpha", 11}, {"beta", 13}};
	Target->Nested.Count = 29;
	auto BeforeRecord = CaptureSnapshot(Target, "Value");
	Target->Value = 91;
	auto AfterRecord = CaptureSnapshot(Target, "Value");
	for (const auto& [SnapshotRecord, Expected] :
		std::array{std::pair{&BeforeRecord, 47}, std::pair{&AfterRecord, 91}})
	{
		Durin::FReflectedValueStorage Storage;
		const auto Result = SnapshotRecord->RestoreDetached(Storage);
		ASSERT_TRUE(Result) << Durin::Editor::FormatTransactionSnapshotError(Result.error());
		EXPECT_EQ(*static_cast<int32*>(Storage.GetValue()), Expected);
	}
	Target->Value = 47;

	for (std::string_view Name : {"Value", "Hard", "Weak", "Soft", "Numbers", "Values", "Nested"})
	{
		const auto Record = CaptureSnapshot(Target, Name);
		Durin::FReflectedValueStorage Storage;
		const auto Result = Record.RestoreDetached(Storage);
		ASSERT_TRUE(Result) << Name << ": " << Durin::Editor::FormatTransactionSnapshotError(Result.error());
		ASSERT_EQ(Storage.GetProperty(), Property(Name));
		if (Name == "Value") EXPECT_EQ(*static_cast<int32*>(Storage.GetValue()), 47);
		if (Name == "Hard")
			EXPECT_EQ(static_cast<Durin::TObjectPtr<Durin::DObject>*>(Storage.GetValue())->Get(), Hard);
		if (Name == "Weak")
			EXPECT_EQ(static_cast<Durin::TWeakObjectPtr<Durin::DObject>*>(Storage.GetValue())->Get(), Weak);
		if (Name == "Soft")
			EXPECT_EQ(static_cast<Durin::TSoftObjectPtr<Durin::DObject>*>(Storage.GetValue())
				->GetPath(), SoftPath);
		if (Name == "Numbers")
			EXPECT_EQ(*static_cast<std::vector<int32>*>(Storage.GetValue()), (std::vector<int32>{3, 5, 8}));
		if (Name == "Values")
			EXPECT_EQ(*static_cast<FRecordMap*>(Storage.GetValue()), Target->Values);
		if (Name == "Nested")
			EXPECT_EQ(static_cast<FRecordNestedValue*>(Storage.GetValue())->Count, 29);
	}

	auto StaleRecord = CaptureSnapshot(Target, "Value");
	TargetRoot = Durin::TStrongObjectPtr<Durin::DObject>(nullptr);
	Durin::MarkAsGarbage(Target);
	Durin::CollectGarbage();
	Durin::FReflectedValueStorage Storage;
	const auto Stale = StaleRecord.RestoreDetached(Storage);
	ASSERT_FALSE(Stale);
	EXPECT_EQ(Stale.error().Code, Durin::Editor::ETransactionSnapshotError::InvalidTarget);
	EXPECT_EQ(Stale.error().Owner, StaleRecord.GetTarget().GetKey());
	EXPECT_EQ(Stale.error().Member, "Value");
}

TEST(FFocusedTransactionObjectSnapshotTests, TypedCaptureFailuresOwnErrorsAndAllowRetry)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	auto* Target = NewObject<DTransactionRecordParticipant>(nullptr, "TypedSnapshotTarget");
	TStrongObjectPtr<DObject> Root(Target);
	Target->Value = 37;
	auto Snapshot = CaptureSnapshot(Target, "Value");
	const auto Before = Snapshot.GetPayload();
	const auto Captured = FTransactionMemberLocator::Capture(Property("Value"), 0);
	ASSERT_TRUE(Captured);
	const auto& Locator = *Captured;
	const auto Null = FTransactionMemberLocator::Capture(nullptr, 0);
	ASSERT_FALSE(Null);
	EXPECT_EQ(Null.error().Code, ETransactionSnapshotError::NullMember);
	EXPECT_EQ(Locator.GetMemberName(), FName("Value"));
	const auto Bounds = FFocusedTransactionObjectSnapshot::Capture(
		Target, Property("Value"), Property("Value")->GetArrayDim());
	ASSERT_FALSE(Bounds);
	EXPECT_EQ(Bounds.error().Code, ETransactionSnapshotError::ArrayIndex);
	EXPECT_EQ(Bounds.error().ArrayIndex, Property("Value")->GetArrayDim());
	EXPECT_EQ(Bounds.error().ArrayDim, Property("Value")->GetArrayDim());
	EXPECT_EQ(Bounds.error().Owner, FObjectKey(Target));
	EXPECT_EQ(Bounds.error().Member, "Value");
	EXPECT_EQ(Snapshot.GetPayload(), Before);
	const auto Resolved = Locator.Resolve(Target);
	ASSERT_TRUE(Resolved);
	EXPECT_EQ(*Resolved, Property("Value"));
	const auto MissingTarget = Locator.Resolve(nullptr);
	ASSERT_FALSE(MissingTarget);
	EXPECT_EQ(MissingTarget.error().Code, ETransactionSnapshotError::InvalidTarget);
	Target->Value = 41;
	auto Retried = FFocusedTransactionObjectSnapshot::Capture(Target, Property("Value"), 0);
	ASSERT_TRUE(Retried);
	Snapshot = std::move(*Retried);
	EXPECT_EQ(Bounds.error().Member, "Value");
	EXPECT_EQ(Bounds.error().Code, ETransactionSnapshotError::ArrayIndex);
	FReflectedValueStorage Storage;
	ASSERT_TRUE(Snapshot.RestoreDetached(Storage));
	EXPECT_EQ(*static_cast<int32*>(Storage.GetValue()), 41);
}

TEST(FFocusedTransactionObjectSnapshotTests, RejectsIncompatibleResolvedMember)
{
	InitializeDObjectSystem();
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(
		nullptr, Durin::FName("IncompatibleRecordTarget"));
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	auto Record = CaptureSnapshot(Target, "Value");
	Durin::FReflectedValueStorage Storage;
	ASSERT_TRUE(Record.RestoreDetached(Storage));
	const void* PreviousStorage = Storage.GetContainer();
	Durin::DClass* Class = DTransactionRecordParticipant::StaticClass();
	Durin::FField* OriginalProperties = Class->ChildProperties;
	Durin::FNumericProperty Incompatible(
		Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
		Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Value),
		sizeof(int64), Durin::DurinCodeGen::EPropertyGenFlags::Int64, nullptr);
	SetValueLifecycle<int64>(Incompatible);
	Incompatible.Next = OriginalProperties->Next;
	Class->ChildProperties = &Incompatible;

	const auto Rejected = Record.RestoreDetached(Storage);
	EXPECT_EQ(Storage.GetContainer(), PreviousStorage);
	Class->ChildProperties = OriginalProperties;
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Code, Durin::Editor::ETransactionSnapshotError::IncompatibleMember);
	EXPECT_EQ(Rejected.error().ExpectedKind, Durin::DurinCodeGen::EPropertyGenFlags::Int32);
	EXPECT_EQ(Rejected.error().ActualKind, Durin::DurinCodeGen::EPropertyGenFlags::Int64);
	EXPECT_EQ(Rejected.error().Owner, Durin::FObjectKey(Target));
	ASSERT_TRUE(Record.RestoreDetached(Storage));
	EXPECT_EQ(Rejected.error().ActualKind, Durin::DurinCodeGen::EPropertyGenFlags::Int64);
	EXPECT_EQ(Rejected.error().Member, "Value");
}

static_assert(!std::is_copy_constructible_v<Durin::Editor::FTransaction>);
static_assert(std::is_move_constructible_v<Durin::Editor::FTransaction>);
static_assert(!std::is_copy_constructible_v<Durin::Editor::FScopedTransaction>);
static_assert(std::is_move_constructible_v<Durin::Editor::FScopedTransaction>);
static_assert(!std::is_constructible_v<Durin::Editor::FTransactionRecord,
	Durin::Editor::FFocusedTransactionObjectSnapshot>);

TEST(FTransBufferTests, ExposesReflectedIdentityAndProductionLimits)
{
	InitializeDObjectSystem();
	EXPECT_TRUE(Durin::DTransactor::StaticClass()->HasAnyClassFlags(
		Durin::EClassFlags::Abstract));
	EXPECT_FALSE(Durin::DTransBuffer::StaticClass()->HasAnyClassFlags(
		Durin::EClassFlags::Abstract));
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "IdentityTransBuffer");
	ASSERT_NE(Buffer, nullptr);
	EXPECT_TRUE(Buffer->IsA<Durin::DTransactor>());
	EXPECT_EQ(Buffer->GetLimits(), (Durin::Editor::FTransactionBufferLimits{}));
	EXPECT_EQ(Buffer->GetLimits().MaximumEntries, 256u);
	EXPECT_EQ(Buffer->GetLimits().MaximumOwnedBytes, 64u * 1024u * 1024u);
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
		NoChange.Modify(Target);
		NoChange.Modify(Target);
		EXPECT_EQ(NoChange.End().Code, Durin::Editor::ETransactorResultCode::NoOp);
	}
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);

	{
		Durin::Editor::FScopedTransaction Change(Buffer, {"test", "Change value"});
		Change.Modify(Target);
		Target->Value = 42;
	}
	ASSERT_EQ(Buffer->GetHistoryCount(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 42);

	{
		Durin::Editor::FScopedTransaction Canceled(Buffer, {"test", "Canceled value"});
		Canceled.Modify(Target);
		Target->Value = 81;
		EXPECT_EQ(Canceled.Cancel().Code, Durin::Editor::ETransactorResultCode::Discarded);
	}
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	EXPECT_EQ(Target->Value, 81);
}

TEST(FTransBufferTests, NestedScopesCancelToSavepointsAndCommitOneEntry)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "NestedTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "NestedTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);

	Durin::Editor::FScopedTransaction Outer(Buffer, {"property", "Nested edit",
		Durin::Editor::FPersistentObjectRef(Target)});
	ASSERT_TRUE(Outer.IsActive());
	Outer.Modify(Target);
	++Target->Value;
	{
		Durin::Editor::FScopedTransaction Inner(Buffer, {"property", "Ignored nested description"});
		ASSERT_TRUE(Inner.IsActive());
		auto* InnerTarget = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "InnerCanceledTarget");
		const auto InnerTargetHandle = Durin::FObjectKey(InnerTarget);
		Inner.Modify(InnerTarget);
		EXPECT_EQ(Inner.Cancel().Code, Durin::Editor::ETransactorResultCode::Discarded);
		EXPECT_FALSE(Inner.IsActive());
		Durin::CollectGarbage();
		EXPECT_EQ(Durin::ResolveObjectKey(InnerTargetHandle), nullptr);
	}
	EXPECT_EQ(Buffer->GetState(), Durin::Editor::ETransactorState::Recording);
	EXPECT_TRUE(Outer.End());
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	EXPECT_EQ(Buffer->GetUndoCount(), 1u);
	EXPECT_EQ(Buffer->GetUndoDescription(), "Nested edit");
	EXPECT_GT(Buffer->GetOwnedBytes(), sizeof(Durin::Editor::FTransaction));
	const auto Events = Buffer->ConsumeEvents();
	ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events[0].Type, Durin::Editor::ETransactionEventType::Executed);
}

TEST(FTransBufferTests, RejectsInvalidCloseOrderAndRecordingBarriersWithoutCorruption)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "BarrierTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	const auto Outer = Buffer->Begin({"test", "Outer"});
	const auto Inner = Buffer->Begin({"test", "Inner"});
	ASSERT_TRUE(Outer);
	ASSERT_TRUE(Inner);
	EXPECT_EQ(Buffer->End(Outer.ScopeId).Code, Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->Undo().Code, Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->Redo().Code, Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->Reset().Code, Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_TRUE(Buffer->End(Inner.ScopeId));
	EXPECT_EQ(Buffer->Cancel(Outer.ScopeId).Code,
		Durin::Editor::ETransactorResultCode::Discarded);
	EXPECT_EQ(Buffer->GetState(), Durin::Editor::ETransactorState::Idle);
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
}

TEST(FTransBufferTests, RejectsRecursiveTransitionStates)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "RecursiveBarrierTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	for (const auto State : {Durin::Editor::ETransactorState::Undoing,
		Durin::Editor::ETransactorState::Redoing})
	{
		Durin::FTransBufferTestAccess::SetState(*Buffer, State);
		EXPECT_EQ(Buffer->Begin({"test", "recursive"}).Code,
			Durin::Editor::ETransactorResultCode::Rejected);
		EXPECT_EQ(Buffer->Undo().Code, Durin::Editor::ETransactorResultCode::Rejected);
		EXPECT_EQ(Buffer->Redo().Code, Durin::Editor::ETransactorResultCode::Rejected);
	}
	Durin::FTransBufferTestAccess::SetState(*Buffer, Durin::Editor::ETransactorState::Idle);
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
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
		First.Modify(Target);
		++Target->Value;
		Durin::Editor::FScopedTransaction Second(std::move(First));
		EXPECT_FALSE(First.IsActive());
		EXPECT_TRUE(Second.IsActive());
	}
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	Durin::Editor::FScopedTransaction Canceled(Buffer, {"test", "Canceled"});
	EXPECT_EQ(Canceled.Cancel().Code, Durin::Editor::ETransactorResultCode::Discarded);
	EXPECT_EQ(Canceled.Cancel().Code, Durin::Editor::ETransactorResultCode::NoOp);
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
}

TEST(FTransBufferTests, MaintainsCursorBranchIdsEventsAndLimits)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "HistoryTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "HistoryTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	auto Commit = [&](std::string Description) {
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", std::move(Description)});
		Scope.Modify(Target);
		++Target->Value;
		return Scope.End();
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
	EXPECT_EQ(Buffer->GetRedoCount(), 0u);
	EXPECT_EQ(Buffer->GetHistoryCount(), 2u);
	EXPECT_GT(Branch.TransactionId, Second.TransactionId);

	ASSERT_TRUE(Buffer->SetLimits({.MaximumEntries = 1, .MaximumOwnedBytes = 64u * 1024u * 1024u}));
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	EXPECT_EQ(Buffer->GetUndoDescription(), "Branch");
	const auto Events = Buffer->ConsumeEvents();
	ASSERT_GE(Events.size(), 8u);
	EXPECT_EQ(Events.back().Type, Durin::Editor::ETransactionEventType::Evicted);

	ASSERT_TRUE(Buffer->Reset());
	const auto AfterReset = Commit("After reset");
	EXPECT_GT(AfterReset.TransactionId, Branch.TransactionId);
}

TEST(FTransBufferTests, DiscardsOversizedEntryAndPreservesFailedUndoCursor)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "FailureTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "FailureTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	ASSERT_TRUE(Buffer->SetLimits({.MaximumEntries = 4, .MaximumOwnedBytes = 1}));
	Durin::Editor::FScopedTransaction Oversized(Buffer, {"test", "Oversized"});
	Oversized.Modify(Target);
	Target->Numbers.push_back(1);
	const auto Rejected = Oversized.End();
	EXPECT_EQ(Rejected.Code, Durin::Editor::ETransactorResultCode::Discarded);
	ASSERT_TRUE(Rejected.FailureCause);
	EXPECT_EQ(Rejected.FailureCause->Code, Durin::Editor::ETransactorFailure::ByteLimit);
	EXPECT_EQ(Rejected.FailureCause->Limit, 1u);
	EXPECT_GT(Rejected.FailureCause->Actual, Rejected.FailureCause->Limit);
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
	EXPECT_EQ(Buffer->GetOwnedBytes(), 0u);

	ASSERT_TRUE(Buffer->SetLimits({.MaximumEntries = 4, .MaximumOwnedBytes = 1024u * 1024u}));
	Durin::Editor::FScopedTransaction Valid(Buffer, {"test", "Stale"});
	Valid.Modify(Target);
	++Target->Value;
	ASSERT_TRUE(Valid.End());
	const auto Id = Buffer->GetUndoId();
	TargetRoot = Durin::TStrongObjectPtr<Durin::DObject>(nullptr);
	Durin::MarkAsGarbage(Target);
	Durin::CollectGarbage();
	EXPECT_EQ(Buffer->Undo().Code, Durin::Editor::ETransactorResultCode::Failed);
	EXPECT_EQ(Buffer->GetUndoId(), Id);
	EXPECT_EQ(Buffer->GetUndoCount(), 1u);
	EXPECT_EQ(Buffer->GetRedoCount(), 0u);
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
		Scope.Modify(Target);
		++Target->Value;
		return Scope.End();
	};
	const auto First = Commit("First");
	const auto Second = Commit("Second");
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);

	EXPECT_EQ(Buffer->Undo(First.TransactionId).Code,
		Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->GetUndoId(), Second.TransactionId);
	EXPECT_EQ(Buffer->GetUndoCount(), 2u);
	ASSERT_TRUE(Buffer->Undo(Second.TransactionId));
	EXPECT_EQ(Buffer->GetRedoId(), Second.TransactionId);
	EXPECT_EQ(Buffer->Redo(First.TransactionId).Code,
		Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->GetRedoId(), Second.TransactionId);

	EXPECT_EQ(Buffer->GetHistoryCount(), 2u);
	EXPECT_EQ(Buffer->GetUndoCount(), 1u);
	ASSERT_TRUE(Buffer->Redo(Second.TransactionId));
	EXPECT_EQ(Buffer->GetUndoId(), Second.TransactionId);
	EXPECT_EQ(Buffer->GetUndoCount(), 2u);
	EXPECT_EQ(Buffer->GetRedoCount(), 0u);
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
		Scope.Modify(Target);
		++Target->Value;
		ASSERT_TRUE(Scope.End());
	}
	const auto UnrelatedId = Buffer->GetUndoId();
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Hard after dependency"});
		Scope.Modify(Target);
		Target->Hard = Participant;
		ASSERT_TRUE(Scope.End());
	}
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Hard before dependency"});
		Scope.Modify(Target);
		Target->Hard = nullptr;
		ASSERT_TRUE(Scope.End());
	}
	ASSERT_TRUE(Buffer->Undo());
	Buffer->ForgetPackage(*Package);
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
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
	ASSERT_TRUE(Buffer->Execute(std::make_unique<FTestCustomChange>(Value, 0, 1, Participant)));
	Buffer->ForgetPackage(*Package);
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
	EXPECT_EQ(Buffer->GetOwnedBytes(), 0u);
	EXPECT_EQ(Value, 1);
	Durin::CollectGarbage();
	EXPECT_FALSE(WeakParticipant.IsValid());
}

TEST(FTransBufferTests, RetainsAnEntryAtTheExactOwnedByteLimit)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ExactLimitTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "ExactLimitTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	auto Commit = [&] {
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Exact limit"});
		Scope.Modify(Target);
		++Target->Value;
		return Scope.End();
	};
	ASSERT_TRUE(Commit());
	const size_t ExactBytes = Buffer->GetOwnedBytes();
	ASSERT_GT(ExactBytes, 0u);
	ASSERT_TRUE(Buffer->Reset());
	ASSERT_TRUE(Buffer->SetLimits({.MaximumEntries = 1, .MaximumOwnedBytes = ExactBytes}));
	EXPECT_TRUE(Commit());
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	EXPECT_EQ(Buffer->GetOwnedBytes(), ExactBytes);
}

TEST(FTransBufferTests, CancellationEvictionAndDestructionReleaseCollectorEdges)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ReleaseTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto MakeTarget = [](std::string_view Name) {
		return Durin::NewObject<DTransactionRecordParticipant>(nullptr, Durin::FName(Name));
	};

	auto* Canceled = MakeTarget("CanceledTarget");
	const auto CanceledHandle = Durin::FObjectKey(Canceled);
	{
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", "Canceled"});
		Scope.Modify(Canceled);
		EXPECT_EQ(Scope.Cancel().Code, Durin::Editor::ETransactorResultCode::Discarded);
	}
	Durin::CollectGarbage();
	EXPECT_EQ(Durin::ResolveObjectKey(CanceledHandle), nullptr);

	ASSERT_TRUE(Buffer->SetLimits({.MaximumEntries = 1, .MaximumOwnedBytes = 1024u * 1024u}));
	auto Commit = [&](DTransactionRecordParticipant* Target, std::string Description) {
		Durin::Editor::FScopedTransaction Scope(Buffer, {"test", std::move(Description)});
		Scope.Modify(Target);
		++Target->Value;
		EXPECT_TRUE(Scope.End());
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
		Scope.Modify(First);
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
		Scope.Modify(Second);
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
	ASSERT_TRUE(Buffer->Execute(std::make_unique<FDetachedChildChange>(Owner, Child, HistoryOuter)));
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
	const auto Execute = Buffer->Execute(std::move(Change));
	ASSERT_TRUE(Execute);
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetUndoId(), Execute.TransactionId);

	ChangePtr->bFailUndo = true;
	EXPECT_EQ(Buffer->Undo().Code, Durin::Editor::ETransactorResultCode::Failed);
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetUndoId(), Execute.TransactionId);
	EXPECT_EQ(Buffer->GetRedoCount(), 0u);
	ChangePtr->bFailUndo = false;
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 2);
}

TEST(FTransBufferTests, DeferredCustomChangeBlocksAndCompletesExactlyOnce)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "DeferredCustomTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	int Value = 5;
	auto Change = std::make_unique<FTestCustomChange>(Value, 3, 5);
	auto* ChangePtr = Change.get();
	ChangePtr->bDeferUndo = true;
	const auto Commit = Buffer->Execute(std::move(Change), true);
	ASSERT_TRUE(Commit);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_TRUE(Buffer->IsTransactionPending(Commit.TransactionId));
	EXPECT_EQ(Buffer->GetState(), Durin::Editor::ETransactorState::Undoing);
	EXPECT_EQ(Buffer->Redo().Code, Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->Begin({"test", "blocked"}).Code,
		Durin::Editor::ETransactorResultCode::Rejected);

	bool bCompletionCalled = false;
	ASSERT_TRUE(Buffer->SetTransactionCompletion(Commit.TransactionId,
		[&](Durin::Editor::FTransactionCompletionResult Result) { bCompletionCalled = static_cast<bool>(Result); }));
	ChangePtr->CompleteUndo({});
	EXPECT_TRUE(bCompletionCalled);
	EXPECT_FALSE(Buffer->IsTransactionPending(Commit.TransactionId));
	EXPECT_EQ(Buffer->GetState(), Durin::Editor::ETransactorState::Idle);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_EQ(Buffer->GetRedoCount(), 1u);
	EXPECT_EQ(Value, 3);
}

TEST(FTransBufferTests, CustomReferencesAndModuleDrainReleaseHistory)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ModuleCustomTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Referenced = Durin::NewObject<Durin::DObject>(nullptr, "CustomReferencedObject");
	const Durin::FObjectKey Handle = Durin::FObjectKey(Referenced);
	int Value = 1;
	ASSERT_TRUE(Buffer->Execute(std::make_unique<FTestCustomChange>(
		Value, 1, 2, Referenced, "TestModule")));
	Durin::CollectGarbage();
	EXPECT_NE(Durin::ResolveObjectKey(Handle), nullptr);
	ASSERT_TRUE(Buffer->DiscardCustomChangesByModule("TestModule"));
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
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
	const auto Result = Buffer->Undo();
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
	EXPECT_EQ(Buffer->GetUndoCount(), 1u);
	EXPECT_FALSE(Buffer->CanUndo());
	EXPECT_FALSE(Buffer->CanRedo());
	EXPECT_EQ(Buffer->Undo().Code, ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->Redo().Code, ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->Reset().Code, ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->Begin({.Description = "Blocked"}).Code, ETransactorResultCode::Rejected);
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
	Transaction.AddRecord(std::unique_ptr<ITransactionCustomChange>{});
	const auto Rejected = Transaction.Apply(false, Durin::EPropertyChangeOrigin::Redo);
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Status, ETransactionApplyFailure::ValidationFailed);
	EXPECT_EQ(Rejected.error().RecordIndex, 1u);
	const auto RejectionMessage = Rejected.error().Message;
	EXPECT_EQ(Value, 1);
	Transaction.TruncateRecords(1);
	const auto Applied = Transaction.Apply(false, Durin::EPropertyChangeOrigin::Redo);
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

TEST(FTransBufferTests, DeferredFailureRetainsMessageAndAllowsRetry)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	auto* Buffer = NewObject<DTransBuffer>(nullptr, "TypedDeferredTransBuffer");
	TStrongObjectPtr<DObject> Root(Buffer);
	int Value = 5;
	auto Change = std::make_unique<FTestCustomChange>(Value, 3, 5);
	auto* ChangePtr = Change.get();
	ChangePtr->bDeferUndo = true;
	const auto Commit = Buffer->Execute(std::move(Change), true);
	ASSERT_TRUE(Commit);
	ASSERT_TRUE(Buffer->Undo());
	std::optional<FTransactionCompletionResult> Retained;
	ASSERT_TRUE(Buffer->SetTransactionCompletion(Commit.TransactionId,
		[&](FTransactionCompletionResult Result) { Retained = std::move(Result); }));
	std::string Message = "Owned deferred failure";
	ChangePtr->CompleteUndo({{.Code = ETransactionCompletionError::Operation, .Message = Message}});
	Message.clear();
	ASSERT_TRUE(Retained);
	EXPECT_FALSE(*Retained);
	EXPECT_EQ(Retained->Error.Code, ETransactionCompletionError::Operation);
	EXPECT_EQ(Retained->Error.TransactionId, Commit.TransactionId);
	EXPECT_EQ(Retained->Error.Operation, ETransactionOperation::Undo);
	EXPECT_EQ(Retained->Error.Message, "Owned deferred failure");
	EXPECT_EQ(Buffer->GetState(), ETransactorState::Idle);
	EXPECT_EQ(Buffer->GetUndoCount(), 1u);
	EXPECT_EQ(Value, 5);
	ASSERT_TRUE(Buffer->Undo());
	bool Completed = false;
	ASSERT_TRUE(Buffer->SetTransactionCompletion(Commit.TransactionId,
		[&](FTransactionCompletionResult Result) { Completed = static_cast<bool>(Result); }));
	ChangePtr->CompleteUndo({});
	EXPECT_TRUE(Completed);
	EXPECT_EQ(Value, 3);
	EXPECT_EQ(Buffer->GetRedoCount(), 1u);
	EXPECT_EQ(Retained->Error.Message, "Owned deferred failure");
}

TEST(FTransBufferTests, TypedRejectionsRetainStateAndRequestedBoundsAcrossRetry)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	auto* Buffer = NewObject<DTransBuffer>(nullptr, "TypedRejectionTransBuffer");
	TStrongObjectPtr<DObject> Root(Buffer);
	const auto Outer = Buffer->Begin({.Description = "Outer"});
	ASSERT_TRUE(Outer);
	const auto Inner = Buffer->Begin({.Description = "Inner"});
	ASSERT_TRUE(Inner);
	const auto Order = Buffer->End(Outer.ScopeId);
	ASSERT_TRUE(Order.RejectionCause);
	EXPECT_EQ(Order.RejectionCause->Reason, ETransactorRejectionReason::CloseOrder);
	EXPECT_EQ(Order.RejectionCause->RequestedId, Outer.ScopeId);
	EXPECT_EQ(Order.RejectionCause->ScopeId, Inner.ScopeId);
	EXPECT_EQ(Order.RejectionCause->PendingId, Outer.TransactionId);
	EXPECT_EQ(Order.RejectionCause->State, ETransactorState::Recording);
	ASSERT_EQ(Buffer->Cancel(Inner.ScopeId).Code, ETransactorResultCode::Discarded);
	ASSERT_EQ(Buffer->Cancel(Outer.ScopeId).Code, ETransactorResultCode::Discarded);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::Idle);
	const auto Limits = Buffer->SetLimits({.MaximumEntries = 0, .MaximumOwnedBytes = 128});
	ASSERT_TRUE(Limits.RejectionCause);
	EXPECT_EQ(Limits.RejectionCause->Reason, ETransactorRejectionReason::ZeroLimits);
	ASSERT_TRUE(Limits.RejectionCause->RequestedLimits);
	EXPECT_EQ(Limits.RejectionCause->RequestedLimits->MaximumEntries, 0u);
	EXPECT_EQ(Limits.RejectionCause->RequestedLimits->MaximumOwnedBytes, 128u);
	int Value = 1;
	const auto Commit = Buffer->Execute(std::make_unique<FTestCustomChange>(Value, 1, 2));
	ASSERT_TRUE(Commit);
	const auto Head = Buffer->Undo(Commit.TransactionId + 1);
	ASSERT_TRUE(Head.RejectionCause);
	EXPECT_EQ(Head.RejectionCause->Reason, ETransactorRejectionReason::UndoHead);
	EXPECT_EQ(Head.RejectionCause->RequestedId, Commit.TransactionId + 1);
	EXPECT_EQ(Head.RejectionCause->UndoHead, Commit.TransactionId);
	EXPECT_EQ(Value, 2);
	ASSERT_TRUE(Buffer->Undo(Commit.TransactionId));
	EXPECT_EQ(Value, 1);
	EXPECT_EQ(Head.RejectionCause->UndoHead, Commit.TransactionId);
	EXPECT_EQ(Order.RejectionCause->ScopeId, Inner.ScopeId);
}

TEST(FTransBufferTests, TypedFailuresRetainAccountingAndExpiredParticipantContext)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	auto* Buffer = NewObject<DTransBuffer>(nullptr, "TypedFailureTransBuffer");
	TStrongObjectPtr<DObject> BufferRoot(Buffer);
	class FOverflowChange final : public ITransactionCustomChange
	{
	public:
		auto GetDescription() const -> std::string_view override { return "Overflow test"; }
		auto Replay(Durin::Editor::ETransactionOperation Operation) -> Durin::Editor::FTransactionCustomResult override
		{
			if (Operation == Durin::Editor::ETransactionOperation::Undo)
			{
				return {};
			}
			else
			{
				return {};
			}
		}
		auto GetAllocatedSize() const -> size_t override { return std::numeric_limits<size_t>::max(); }
	};
	const auto Overflow = Buffer->Execute(std::make_unique<FOverflowChange>(), true);
	EXPECT_EQ(Overflow.Code, ETransactorResultCode::Failed);
	ASSERT_TRUE(Overflow.FailureCause);
	EXPECT_EQ(Overflow.FailureCause->Code, ETransactorFailure::EntryAccounting);
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::Idle);
	auto* Target = NewObject<DTransactionRecordParticipant>(nullptr, "ExpiredModifiedParticipant");
	TStrongObjectPtr<DObject> TargetRoot(Target);
	const FObjectKey Key(Target);
	FScopedTransaction Scope(Buffer, {.Description = "Expired target"});
	Scope.Modify(Target);
	MarkAsGarbage(Target);
	const auto Inner = Buffer->Begin({.Description = "Blocks cleanup"});
	ASSERT_TRUE(Inner);
	const auto BlockedCleanup = Scope.End();
	EXPECT_EQ(BlockedCleanup.Code, ETransactorResultCode::Rejected);
	ASSERT_TRUE(BlockedCleanup.RejectionCause);
	EXPECT_EQ(BlockedCleanup.RejectionCause->Reason, ETransactorRejectionReason::CloseOrder);
	ASSERT_TRUE(BlockedCleanup.PreparationCause);
	ASSERT_TRUE(BlockedCleanup.PreparationCause->FailureCause);
	EXPECT_EQ(BlockedCleanup.PreparationCause->FailureCause->Code, ETransactorFailure::ExpiredObject);
	EXPECT_EQ(Buffer->Cancel(Inner.ScopeId).Code, ETransactorResultCode::Discarded);
	const auto Expired = Scope.End();
	EXPECT_EQ(Expired.Code, ETransactorResultCode::Failed);
	ASSERT_TRUE(Expired.FailureCause);
	EXPECT_EQ(Expired.FailureCause->Code, ETransactorFailure::ExpiredObject);
	EXPECT_EQ(Expired.FailureCause->Owner, Key);
	ASSERT_TRUE(Expired.CleanupCause);
	EXPECT_EQ(Expired.CleanupCause->Code, ETransactorResultCode::Discarded);
	EXPECT_EQ(Scope.Cancel().Code, ETransactorResultCode::NoOp);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::Idle);
	EXPECT_EQ(Overflow.FailureCause->Code, ETransactorFailure::EntryAccounting);
}
