#include "Editor/EditorTransactionTestSupport.h"
#include "Editor/EditorCommand.h"
#include "Editor/EditorEngine.h"
#include "Actors/ActorChange.h"
#include "Transactions/TransactionRecord.h"
#include "PropertyEditor/PropertyEditing.h"
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
		int32 Session = 0;
		int32 Ignored = 0;
		Durin::TObjectPtr<Durin::DObject> IgnoredReference;
	};

	using FRecordMap = std::unordered_map<std::string, int32>;

	class DTransactionRecordParticipant final : public Durin::DObject
	{
	public:
		explicit DTransactionRecordParticipant(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{
			SetFlags(Durin::EObjectFlags::Transactional);
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
			auto* NestedSession = new Durin::FNumericProperty(Durin::FFieldVariant(NestedStruct), "Session", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Transient, 1, offsetof(FRecordNestedValue, Session), sizeof(int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetValueLifecycle<int32>(*NestedSession);
			auto* NestedIgnored = new Durin::FNumericProperty(Durin::FFieldVariant(NestedStruct), "Ignored", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::NonTransactional, 1, offsetof(FRecordNestedValue, Ignored), sizeof(int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetValueLifecycle<int32>(*NestedIgnored);
			auto* NestedReference = new Durin::FObjectProperty(Durin::FFieldVariant(NestedStruct), "IgnoredReference", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::NonTransactional, 1, offsetof(FRecordNestedValue, IgnoredReference), sizeof(Durin::TObjectPtr<Durin::DObject>),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(), true,
				[](const void* Value) -> Durin::DObject* { return static_cast<const Durin::TObjectPtr<Durin::DObject>*>(Value)->Get(); },
				[](void* Value, Durin::DObject* Object) { *static_cast<Durin::TObjectPtr<Durin::DObject>*>(Value) = Object; });
			SetValueLifecycle<Durin::TObjectPtr<Durin::DObject>>(*NestedReference);
			NestedCount->SetNext(NestedSession); NestedSession->SetNext(NestedIgnored); NestedIgnored->SetNext(NestedReference);
			NestedStruct->SetChildProperties(NestedCount);
			auto* NestedProperty = new Durin::FStructProperty(
				Durin::FFieldVariant(Class), Durin::FName("Nested"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Nested),
				NestedStruct);

			auto* SessionProperty = new Durin::FNumericProperty(Durin::FFieldVariant(Class), "SessionValue", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Transient, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, SessionValue), sizeof(int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetValueLifecycle<int32>(*SessionProperty);
			auto* UnrecordedProperty = new Durin::FNumericProperty(Durin::FFieldVariant(Class), "UnrecordedValue", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::NonTransactional, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, UnrecordedValue), sizeof(int32), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetValueLifecycle<int32>(*UnrecordedProperty);
			ValueProperty->SetNext(SessionProperty);
			SessionProperty->SetNext(UnrecordedProperty);
			UnrecordedProperty->SetNext(HardProperty);
			HardProperty->SetNext(WeakProperty);
			WeakProperty->SetNext(SoftProperty);
			SoftProperty->SetNext(ArrayProperty);
			ArrayProperty->SetNext(MapProperty);
			MapProperty->SetNext(NestedProperty);
			Class->SetChildProperties(ValueProperty);
			Class->Register(Durin::DClass::StaticClass, "", "DTransactionRecordParticipant");
			Durin::DObjectForceRegistration(Class);
			return Class;
		}

		auto PreEditChangeProperty(Durin::FPropertyEditProposal& Proposal) -> std::expected<void, Durin::FObjectValidationError> override
		{
			++ValidationCalls;
			if (bRejectNewEdits) return Durin::RejectPropertyEdit(*this, Proposal, Durin::EPropertyEditRejection::Rejected);
			return {};
		}
		bool bRejectNewEdits = false;
		int32 ValidationCalls = 0;
		int32 Value = 0;
		int32 SessionValue = 0;
		int32 UnrecordedValue = 0;
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

	struct FRecordingStart
	{
		int32 Index = -1;
		Durin::Editor::FTransactionId TransactionId = 0;
		explicit operator bool() const { return Index >= 0; }
	};
	auto BeginRecording(Durin::DTransactor& Buffer, Durin::Editor::FTransactionContext Context = {}) -> FRecordingStart
	{
		const auto Index = Buffer.Begin(Context.Name, Context.Description);
		Buffer.SetPrimaryUndoObject(Context.PrimaryObject.Resolve());
		return {Index, Buffer.GetLastResult().TransactionId};
	}
	auto EndRecording(Durin::Editor::FScopedTransaction& Scope, Durin::DTransactor& Buffer) -> Durin::Editor::FTransactorResult
	{ (void)Scope.End(); return Buffer.GetLastResult(); }
	auto CancelRecording(Durin::Editor::FScopedTransaction& Scope, Durin::DTransactor& Buffer) -> Durin::Editor::FTransactorResult
	{ Scope.Cancel(); return Buffer.GetLastResult(); }
	auto CancelRecording(Durin::DTransactor& Buffer, int32 Index) -> Durin::Editor::FTransactorResult
	{ Buffer.Cancel(Index); return Buffer.GetLastResult(); }
	auto EndRecording(Durin::DTransactor& Buffer) -> Durin::Editor::FTransactorResult
	{ (void)Buffer.End(); return Buffer.GetLastResult(); }
	auto ReadTransactionEvents(Durin::DTransactor& Buffer) -> std::vector<Durin::Editor::FTransactionEvent>
	{
		std::vector<Durin::Editor::FTransactionEvent> Events;
		const auto Handle = Buffer.AddTransactionListener([&](const auto& Event) { Events.push_back(Event); });
		Buffer.DispatchEvents(); Buffer.RemoveTransactionListener(Handle);
		return Events;
	}

	class FTestCustomChange final : public Durin::Editor::FActorCommandChange
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
		auto Replay(Durin::Editor::ETransactionOperation Operation) -> Durin::Editor::FActorChangeResult override
		{
			if (ReplayProbe) ReplayProbe();
			if (Operation == Durin::Editor::ETransactionOperation::Undo)
			{
				if (bFailUndo) { return {{.Code = Durin::Editor::EActorChangeError::InjectedMutation}}; }
				Value = Before;
				bOverflowed = false;
				return {};
			}
			else
			{
				if (bFailRedo)
				{
					return {{.Code = Durin::Editor::EActorChangeError::InjectedMutation,
						.CleanupCause = bFailCleanup ? std::make_shared<Durin::Editor::FActorChangeError>(
							Durin::Editor::FActorChangeError{.Code = Durin::Editor::EActorChangeError::RollbackIncomplete}) : nullptr}};
				}
				if (bGrowOnRedo) Details.resize(8192, 'x');
				bOverflowed = bOverflowOnRedo;
				Value = After;
				if (bThrowRedo) throw std::runtime_error("replay exception fixture");
				return {};
			}
		}
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) const -> void override
		{
			Durin::DObject* Object = Reference.Get();
			if (Object) Collector.AddReferencedObject(Object);
		}
		auto GetAllocatedSize() const -> size_t override
		{
			return bOverflowed ? std::numeric_limits<size_t>::max() : Details.capacity() + Module.capacity();
		}
		auto GetOwningModule() const -> std::string_view override { return Module; }

		int& Value;
		int Before = 0;
		int After = 0;
		Durin::TObjectPtr<Durin::DObject> Reference;
		std::string Module;
		std::string Details;
		bool bFailUndo = false;
		bool bFailRedo = false;
		bool bFailCleanup = false;
		bool bGrowOnRedo = false;
		bool bOverflowOnRedo = false, bOverflowed = false;
		bool bThrowRedo = false;
		std::function<void()> ReplayProbe;
	};

	// Qualifies expression-style deletion: history owns the detached child, not a garbage object.
	class FDetachedChildChange final : public Durin::Editor::FActorCommandChange
	{
	public:
		FDetachedChildChange(DTransactionRecordParticipant* InOwner, Durin::DObject* InChild,
			Durin::DObject* InHistoryOuter)
			: Owner(InOwner), Child(InChild), HistoryOuter(InHistoryOuter) {}

		auto GetDescription() const -> std::string_view override { return "Detach owned child"; }
		auto Replay(Durin::Editor::ETransactionOperation Operation) -> Durin::Editor::FActorChangeResult override
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
	Durin::FField* OriginalProperties = Class->GetChildProperties();
	Durin::FNumericProperty Incompatible(
		Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
		Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, Value),
		sizeof(int64), Durin::DurinCodeGen::EPropertyGenFlags::Int64, nullptr);
	SetValueLifecycle<int64>(Incompatible);
	Incompatible.SetNext(OriginalProperties->GetNext());
	Class->SetChildProperties(&Incompatible);

	const auto Rejected = Record.RestoreDetached(Storage);
	EXPECT_EQ(Storage.GetContainer(), PreviousStorage);
	Class->SetChildProperties(OriginalProperties);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);

	{
		Durin::Editor::FScopedTransaction Change(Buffer, {"test", "Change value"});
		(void)Target->Modify();
		Target->Value = 42;
	}
	ASSERT_EQ(Buffer->GetHistoryCount(), 1u);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	EXPECT_EQ(Target->Value, 81);
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
	ASSERT_EQ(Buffer->GetHistoryCount(), 1u);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	Durin::Editor::FScopedTransaction Canceled(Buffer, {"test", "Canceled"});
	EXPECT_EQ(CancelRecording(Canceled, *Buffer).Code, Durin::Editor::ETransactorResultCode::Discarded);
	EXPECT_FALSE(Canceled.IsOutstanding());
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
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
	EXPECT_EQ(Buffer->GetRedoCount(), 0u);
	EXPECT_EQ(Buffer->GetHistoryCount(), 2u);
	EXPECT_GT(Branch.TransactionId, Second.TransactionId);

	EXPECT_EQ(Buffer->GetHistoryCount(), 2u);
	EXPECT_EQ(Buffer->GetUndoDescription(), "Branch");
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
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
	EXPECT_EQ(Buffer->GetUndoCount(), 2u);
	ASSERT_TRUE(Buffer->UndoExpected(Second.TransactionId));
	EXPECT_EQ(Buffer->GetRedoId(), Second.TransactionId);
	EXPECT_EQ(Buffer->RedoExpected(First.TransactionId).Code,
		Durin::Editor::ETransactorResultCode::Rejected);
	EXPECT_EQ(Buffer->GetRedoId(), Second.TransactionId);

	EXPECT_EQ(Buffer->GetHistoryCount(), 2u);
	EXPECT_EQ(Buffer->GetUndoCount(), 1u);
	ASSERT_TRUE(Buffer->RedoExpected(Second.TransactionId));
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
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1, Participant)));
	Buffer->ForgetPackage(*Package);
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
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
	EXPECT_EQ(Buffer->GetRedoCount(), 0u);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
	ASSERT_TRUE(EndRecording(*Buffer));
	auto Second = std::make_unique<FTestCustomChange>(Value, 2, 3);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::move(Second)));
	EXPECT_EQ(Buffer->UndoExpected(Durin::Editor::FTransactionId{0}).Code, ETransactorResultCode::Rejected);
	ASSERT_TRUE(EndRecording(*Buffer));
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	EXPECT_EQ(Buffer->GetUndoDescription(), "Grouped edit");
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
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
	EXPECT_EQ(Buffer->GetRedoCount(), 0u);
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
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
	EXPECT_EQ(Buffer->GetUndoCount(), 1u);
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
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "SoftBudgetTransBuffer"));
	FTransBufferTestAccess::InitializeMemoryBudget(*Buffer, 2048);
	int Value = 0;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	auto Growing = std::make_unique<FTestCustomChange>(Value, 1, 2);
	Growing->bGrowOnRedo = true;
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::move(Growing)));
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetHistoryCount(), 2u);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 2u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 2);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 2, 3)));
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 300u);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	ASSERT_TRUE(Durin::Editor::ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 2, 3)));
	EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 2);
}

TEST(FTransBufferTests, RedoBranchReplacementDoesNotAlsoTrimTheUndoPrefix)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer("OverBudgetRedo", 2048);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 2u);
	EXPECT_EQ(Buffer->GetRedoCount(), 0u);
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u); EXPECT_EQ(Target->Value, 2);
	EXPECT_FALSE(HasObjectTransactionBinding());
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
	EXPECT_EQ(Buffer->GetHistoryCount(), 0u); EXPECT_TRUE(Package->IsDirty());
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

TEST(FTransBufferTests, FocusedRecordsAndModifyShareFirstBeforeAndFinalAfter)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SharedMemberRecord"));
	FPropertyValueSnapshotPayload Before, After;
	ASSERT_TRUE(CapturePropertyValuePayload(Property("Value"), Target.Get(), 0, Before));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Mixed property recording"});
		Target->Value = 1;
		ASSERT_TRUE(CapturePropertyValuePayload(Property("Value"), Target.Get(), 0, After));
		auto Record = FTransactionObjectRecord::Capture(FPropertyEditTarget::ForMember(Target.Get(), Property("Value")), Before, After);
		ASSERT_TRUE(Record); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Record)));
		ASSERT_TRUE(Target->Modify()); Target->Value = 3;
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 3);
}

TEST(FTransBufferTests, UndoWithoutRedoDiscardsTheRedoSuffix)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Undo(false));
	EXPECT_EQ(Value, 0); EXPECT_EQ(Buffer->GetHistoryCount(), 0u); EXPECT_FALSE(Buffer->CanRedo());
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
		FScopedTransaction Scope(Buffer.Get(), {"test", "Unrelated context", FPersistentObjectRef(Primary.Get())});
		ASSERT_TRUE(Target->Modify()); Target->Value = 1;
	}
	EXPECT_FALSE(PrimaryPackage->IsDirty());
	const auto* Transaction = Buffer->GetTransaction(0); ASSERT_NE(Transaction, nullptr);
	EXPECT_EQ(Transaction->GetContext().PrimaryObject.Resolve(), Primary.Get());
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
	const auto Count = Buffer->GetHistoryCount();
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Excluded field only"}); ASSERT_TRUE(Target->Modify()); Target->Nested.Ignored = 7; }
	EXPECT_EQ(Buffer->GetHistoryCount(), Count);
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
	EXPECT_EQ(Result.Code, ETransactorResultCode::Failed); EXPECT_EQ(Value, 0); EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
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
	EXPECT_EQ(Value, 1); EXPECT_FALSE(HasObjectTransactionBinding());
}

TEST(FTransBufferTests, ModuleRetirementCannotEraseAReplayingRecord)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0; auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1, nullptr, "ReplayOwnedModule"); auto* Retained = Change.get();
	ASSERT_TRUE(ApplyEditorChange(*Buffer, std::move(Change)));
	Retained->ReplayProbe = [&] {
		EXPECT_EQ(Buffer->DiscardCustomChangesByModule("ReplayOwnedModule").Code, ETransactorResultCode::Rejected);
		EXPECT_EQ(Buffer->GetHistoryCount(), 1u);
	};
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Value, 0);
	ASSERT_TRUE(Buffer->DiscardCustomChangesByModule("ReplayOwnedModule")); EXPECT_EQ(Buffer->GetHistoryCount(), 0u);
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
		const auto Id = Buffer->GetRecordingId();
		EXPECT_FALSE(Old.IsOutstanding());
		if (CancelOld) Old.Cancel(); else EXPECT_EQ(0, Old.End());
		EXPECT_EQ(Id, Buffer->GetRecordingId());
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
	EXPECT_EQ(nullptr, GetObjectTransactionRecorder());
}

TEST(FTransBufferTests, HostExecutionBindingSupportsABAAndNonLifoRecordingCompletion)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	TStrongObjectPtr<DEditorEngine> A(NewObject<DEditorEngine>(nullptr, "ExecutionHostA"));
	TStrongObjectPtr<DEditorEngine> B(NewObject<DEditorEngine>(nullptr, "ExecutionHostB"));
	TStrongObjectPtr<DTransactionRecordParticipant> Left(NewObject<DTransactionRecordParticipant>(nullptr, "ExecutionLeft"));
	TStrongObjectPtr<DTransactionRecordParticipant> Right(NewObject<DTransactionRecordParticipant>(nullptr, "ExecutionRight"));
	std::unique_ptr<FScopedTransaction> First, Second;
	A->DispatchSubsystemCallback([&] {
		First = std::make_unique<FScopedTransaction>("A");
		ASSERT_TRUE(Left->Modify()); Left->Value = 1;
	});
	B->DispatchSubsystemCallback([&] {
		EXPECT_FALSE(Right->Modify(false)); // An idle host never joins A's suspended recording.
		Second = std::make_unique<FScopedTransaction>("B");
		ASSERT_TRUE(Right->Modify()); Right->Value = 2;
	});
	EXPECT_FALSE(Left->Modify(false)); // Ambiguous unbound entry cannot select a host.
	A->DispatchSubsystemCallback([&] {
		FScopedTransaction Inner("A inner");
		ASSERT_TRUE(Left->Modify()); Left->Value = 3;
		B->DispatchSubsystemCallback([&] { ASSERT_TRUE(Right->Modify()); Right->Value = 4; });
		ASSERT_TRUE(Left->Modify()); Left->Value = 5;
	});
	EXPECT_EQ(1, First->End()); // A completes before the later-opened B.
	EXPECT_TRUE(Second->IsOutstanding());
	EXPECT_TRUE(A->GetTransactor()->Undo()); EXPECT_EQ(0, Left->Value); EXPECT_EQ(4, Right->Value);
	EXPECT_TRUE(A->GetTransactor()->Redo()); EXPECT_EQ(5, Left->Value);
	EXPECT_EQ(1, Second->End());
	EXPECT_TRUE(B->GetTransactor()->Undo()); EXPECT_EQ(0, Right->Value); EXPECT_EQ(5, Left->Value);
	EXPECT_TRUE(B->GetTransactor()->Redo()); EXPECT_EQ(4, Right->Value);
	First.reset(); Second.reset();
	MarkObjectHierarchyAsGarbage(A.Get()); MarkObjectHierarchyAsGarbage(B.Get());
	A.Reset(); B.Reset(); CollectGarbage();
}

TEST(FTransBufferTests, HostRetirementCannotRestoreItsRecorderOverAnotherHost)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner A("RetiringA"), B("SurvivingB");
	FScopedTransaction First(A.Get(), {.Description = "A"}), Second(B.Get(), {.Description = "B"});
	{
		FScopedObjectTransactionBinding Outer(A->GetObjectTransactionRecorder());
		{
			FScopedObjectTransactionBinding Inner(B->GetObjectTransactionRecorder());
			A->BeginDestroy();
			EXPECT_EQ(B->GetObjectTransactionRecorder().get(), GetObjectTransactionRecorder());
		}
		EXPECT_EQ(nullptr, GetObjectTransactionRecorder());
	}
	EXPECT_FALSE(First.IsOutstanding());
	EXPECT_TRUE(Second.IsOutstanding());
	EXPECT_EQ(B->GetObjectTransactionRecorder().get(), GetObjectTransactionRecorder());
	EXPECT_EQ(1, Second.End());
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
	EXPECT_EQ(nullptr, GetObjectTransactionRecorder());
}

TEST(FTransBufferTests, HostMarkedGarbageCannotRestoreRecorderBeforeDeferredDestruction)
{
	using namespace Durin;
	using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner A("GarbagePrior"), B("LiveCurrent");
	FScopedTransaction First(A.Get(), {.Description = "A"}), Second(B.Get(), {.Description = "B"});
	{
		FScopedObjectTransactionBinding Outer(A->GetObjectTransactionRecorder());
		{
			FScopedObjectTransactionBinding Inner(B->GetObjectTransactionRecorder());
			MarkObjectHierarchyAsGarbage(A.Get());
			EXPECT_EQ(B->GetObjectTransactionRecorder().get(), GetObjectTransactionRecorder());
		}
		EXPECT_EQ(nullptr, GetObjectTransactionRecorder());
	}
	EXPECT_FALSE(First.IsOutstanding());
	EXPECT_EQ(B->GetObjectTransactionRecorder().get(), GetObjectTransactionRecorder());
	EXPECT_EQ(1, Second.End());
	CollectGarbage();
}
