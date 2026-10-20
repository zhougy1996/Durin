#include "Editor/EditorTransactionTestSupport.h"
#include "Transactions/Transactor.h"
#include "Editor/EditorEngine.h"
#include "Actors/ActorChange.h"
#include "PropertyEditor/PropertyEditing.h"

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
#include "DObject/TransactionObjectAnnotation.h"
#include "DObject/ObjectGraphReplacement.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/Actor.h"
#include "ComponentReregisterContext.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Engine/TransactionAnnotations.h"
#include "EngineTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	template<typename T, typename Base>
	auto MakeTransactionEngineTestClass(const char* Name) -> Durin::DClass*
	{
		using namespace Durin;
		auto* Class = new DClass(EC_StaticConstructor, Name, sizeof(T), alignof(T), EObjectFlags::NoFlags,
			EClassFlags::NoClassDefaultObject, EClassCastFlags::DClass, (DClass::ClassConstructorType)InternalConstructor<T>);
		Class->SetSuperStructBase(Base::StaticClass()); Class->SetTypeNames(Name, "", "");
		Class->Register(DClass::StaticClass, "", Name); DObjectForceRegistration(Class); return Class;
	}

	struct FUnsupportedSceneInstanceData : Durin::FSceneComponentInstanceData
	{
		using FSceneComponentInstanceData::FSceneComponentInstanceData;
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		int32 DomainValue = 17;
	};

	class DTransactionLifecycleScene final : public Durin::DSceneComponent
	{
	public:
		explicit DTransactionLifecycleScene(const Durin::FObjectInitializer& Initializer) : DSceneComponent(Initializer) { SetRelativeLocation(NativeInitialLocation); }
		inline static Durin::FVector3 NativeInitialLocation{0.0};
		static void __DefaultConstructor(const Durin::FObjectInitializer& Initializer) { new (Initializer.GetObj()) DTransactionLifecycleScene(Initializer); }
		static auto StaticClass() -> Durin::DClass* { static auto* Class = MakeTransactionEngineTestClass<DTransactionLifecycleScene, Durin::DSceneComponent>("TransactionLifecycleScene"); return Class; }
		auto OnComponentCreated() -> void override { DSceneComponent::OnComponentCreated(); ++Created; CreatedLocation = GetWorldLocation(); }
		auto OnComponentDestroyed() -> void override { DSceneComponent::OnComponentDestroyed(); ++Destroyed; }
		auto OnRegister() -> void override
		{
			DSceneComponent::OnRegister(); ++Registered;
			if (RegistrationCallback) RegistrationCallback();
			if (bCollectOnRegister) Durin::CollectGarbage();
			if (bThrowOnRegister) throw std::runtime_error("Injected registration callback failure.");
			if (bDestroyOnRegister) DestroyComponent();
		}
		auto OnUnregister() -> void override { DSceneComponent::OnUnregister(); ++Unregistered; }
		auto BeginPlay() -> void override { DSceneComponent::BeginPlay(); ++Begun; }
		auto EndPlay() -> void override { DSceneComponent::EndPlay(); ++Ended; }
		auto InitializeComponent() -> void override { DSceneComponent::InitializeComponent(); ++Initialized; }
		auto UninitializeComponent() -> void override { DSceneComponent::UninitializeComponent(); ++Uninitialized; }
		auto GetComponentInstanceData() const -> std::shared_ptr<Durin::FActorComponentInstanceData> override
		{
			if (bUnsupportedInstanceData) return std::make_shared<FUnsupportedSceneInstanceData>(*this);
			return DSceneComponent::GetComponentInstanceData();
		}
		uint32 Created = 0, Destroyed = 0, Registered = 0, Unregistered = 0, Initialized = 0, Uninitialized = 0, Begun = 0, Ended = 0;
		Durin::FVector3 CreatedLocation{0.0};
		bool bCollectOnRegister = false, bDestroyOnRegister = false, bThrowOnRegister = false;
		bool bUnsupportedInstanceData = false;
		std::function<void()> RegistrationCallback;
	};

	class DTransactionConstructionActor final : public Durin::AActor
	{
	public:
		explicit DTransactionConstructionActor(const Durin::FObjectInitializer& Initializer) : AActor(Initializer)
		{
			SetRootComponent(CreateDefaultComponent<DTransactionLifecycleScene>("NativeRoot"));
		}
		static void __DefaultConstructor(const Durin::FObjectInitializer& Initializer) { new (Initializer.GetObj()) DTransactionConstructionActor(Initializer); }
		static auto StaticClass() -> Durin::DClass* { static auto* Class = MakeTransactionEngineTestClass<DTransactionConstructionActor, Durin::AActor>("TransactionConstructionActor"); return Class; }
		static auto Key(uint32 Index) -> Durin::FActorGeneratedComponentKey { return {"TransactionGenerated", Durin::FGuid(0x5452414e, 0x53414354, 0, Index + 1)}; }
		auto Serialize(Durin::FArchive& Ar) -> void override
		{
			AActor::Serialize(Ar);
			auto Scope = Durin::EnterArchiveField(Ar, {StaticClass()->GetQualifiedName(), "GeneratedCount", Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32)});
			Ar << GeneratedCount;
		}
		int32 GeneratedCount = 1;
		uint32 ConstructionCalls = 0;
		bool bFailConstruction = false, bCollectDuringConstruction = false;
		std::optional<Durin::FVector3> ConstructionLocation;
	protected:
		auto OnConstruction(const Durin::FTransform&) -> void override
		{
			++ConstructionCalls;
			for (int32 Index = 0; Index < GeneratedCount; ++Index)
			{
				auto* Component = AddComponentByClass(DTransactionLifecycleScene::StaticClass(), false, Durin::FTransform(), true, Key(Index), "Generated");
				if (!Component) throw std::runtime_error("Component creation failed.");
				if (ConstructionLocation) static_cast<Durin::DSceneComponent*>(Component)->SetRelativeLocation(*ConstructionLocation);
				FinishAddComponent(Component, false, Durin::FTransform());
			}
			if (bCollectDuringConstruction) Durin::CollectGarbage();
			if (bFailConstruction) throw std::runtime_error("Injected transaction reconstruction failure.");

		}
	};

	struct FTransactionEngineWorld
	{
		FTransactionEngineWorld(Durin::EWorldType Type = Durin::EWorldType::Editor)
		{
			InitializeDObjectSystem();
			World = Durin::TStrongObjectPtr<Durin::DWorld>(Durin::NewObject<Durin::DWorld>(nullptr, "TransactionAnnotationWorld"));
			World->SetWorldType(Type);
			EXPECT_TRUE(World->InitializeSubsystems());
			EXPECT_TRUE(World->SetCurrentLevel(Durin::NewObject<Durin::DLevel>(World.Get(), "Level")));
		}
		Durin::TStrongObjectPtr<Durin::DWorld> World;
	};

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
	using FRecordStructMap = std::unordered_map<std::string, FRecordNestedValue>;

	class FRecordTestAnnotation final : public Durin::ITransactionObjectAnnotation
	{
	public:
		FRecordTestAnnotation(int32 InValue, Durin::DObject* InReference, size_t InBytes = sizeof(FRecordTestAnnotation))
			: Value(InValue), Reference(InReference), Bytes(InBytes) {}
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) const -> void override { Reference.AddReferencedObjects(Collector); }
		auto Serialize(Durin::FArchive& Archive) -> void override { Archive << Value; }
		auto GetAllocatedSize() const -> size_t override { return Bytes; }
		auto ComputeAdditionalObjectChanges(const Durin::ITransactionObjectAnnotation* Original) const
			-> std::vector<std::pair<Durin::DObject*, Durin::FTransactionObjectChange>> override
		{
			const auto* Previous = dynamic_cast<const FRecordTestAnnotation*>(Original);
			if (!bReportChanges || !Reference.Resolve() || (Previous && Previous->Value == Value)) return {};
			Durin::FTransactionObjectChange Change(Durin::CaptureTransactionObjectIdentity(*Reference.Resolve()), Reference.Resolve()->GetObjectFlags()); Change.bNonPropertyDataChanged = true;
			return {{Reference.Resolve(), std::move(Change)}};
		}
		size_t Bytes;
		bool bReportChanges = false;
		int32 Value;
		Durin::FPersistentObjectRef Reference;
	};

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
			auto* NestedArray = new Durin::FArrayProperty(Durin::FFieldVariant(Class), "NestedArray", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, NestedArray),
				sizeof(std::vector<FRecordNestedValue>), Durin::DurinCodeGen::EPropertyGenFlags::Array,
				nullptr, Durin::ResolveArrayOps<std::vector<FRecordNestedValue>>());
			NestedArray->SetInner(new Durin::FStructProperty(Durin::FFieldVariant(), "Element", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::None, 1, 0, NestedStruct));
			SetValueLifecycle<std::vector<FRecordNestedValue>>(*NestedArray);
			auto* NestedMap = new Durin::FMapProperty(Durin::FFieldVariant(Class), "NestedMap", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1, STRUCT_OFFSET_UINT16(DTransactionRecordParticipant, NestedMap),
				sizeof(FRecordStructMap), Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, Durin::ResolveMapOps<FRecordStructMap>());
			auto* NestedKey = new Durin::FStringProperty(Durin::FFieldVariant(), "Key", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::None, 1, 0, sizeof(std::string), Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr);
			SetValueLifecycle<std::string>(*NestedKey);
			NestedMap->SetKeyProp(NestedKey);
			NestedMap->SetValueProp(new Durin::FStructProperty(Durin::FFieldVariant(), "Value", Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::None, 1, 0, NestedStruct));
			SetValueLifecycle<FRecordStructMap>(*NestedMap);
			NestedProperty->SetNext(NestedArray); NestedArray->SetNext(NestedMap);
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
		auto Serialize(Durin::FArchive& Archive) -> void override
		{
			if (CaptureTrace && Archive.IsSaving() && Archive.IsTransacting()) CaptureTrace->push_back("serialize");
			DObject::Serialize(Archive);
			if (Archive.IsSaving() && Archive.IsTransacting()) ++SerializationSaves;
			if (Archive.IsLoading() && Archive.IsTransacting()) ++SerializationLoads;
			auto Field = Durin::EnterArchiveField(Archive, {StaticClass()->GetQualifiedName(), "NativeValue",
				Durin::FArchiveLogicalTypeDescriptor::Scalar(true, 32), 1});
			Archive << NativeValue;
			if (Archive.IsLoading() && (bFailLoadOnce || FailedLoadsRemaining > 0))
			{
				bFailLoadOnce = false;
				if (FailedLoadsRemaining > 0) --FailedLoadsRemaining;
				Archive.SetError("Injected transaction object load failure.");
			}
			if (Archive.IsLoading() && bThrowLoadOnce)
			{ bThrowLoadOnce = false; throw std::runtime_error("Injected native transaction load exception."); }
		}
		int32 NativeValue = 0;
		int32 SerializationSaves = 0;
		int32 SerializationLoads = 0;
		bool bFailLoadOnce = false;
		bool bThrowLoadOnce = false;
		int32 FailedLoadsRemaining = 0;
		bool bRejectNewEdits = false;
		bool bThrowPropertyNotification = false;
		auto PostEditChangeProperty(const Durin::FPropertyChangedEvent& Event) -> void override
		{
			if (bThrowPropertyNotification) throw std::runtime_error("Injected property observer failure.");
			DObject::PostEditChangeProperty(Event);
		}
		auto PreEditUndo() -> void override { ++UndoBegins; if (Trace) Trace("pre"); }
		auto PostEditUndo() -> void override { ++UndoCompletions; DObject::PostEditUndo(); if (Trace) Trace("post"); }
		auto PostTransacted(const Durin::FTransactionObjectEvent& Event) -> void override { if (TransactionObserver) TransactionObserver(Event); }
		std::function<void(std::string_view)> Trace;
		std::function<void(const Durin::FTransactionObjectEvent&)> TransactionObserver;
		auto FindOrCreateTransactionAnnotation() const -> std::shared_ptr<Durin::ITransactionObjectAnnotation> override
		{
			if (!bAnnotationEnabled) return {};
			++AnnotationCaptures;
			if (CaptureTrace) CaptureTrace->push_back("annotation");
			auto Annotation = std::make_shared<FRecordTestAnnotation>(SupplementalValue, AnnotationReference, AnnotationBytes);
			Annotation->bReportChanges = bReportAnnotationChanges;
			if (bCollectDuringAnnotation) Durin::CollectGarbage();
			return Annotation;
		}
		auto PostEditUndo(std::shared_ptr<Durin::ITransactionObjectAnnotation> Annotation) -> void override
		{
			if (auto Value = std::dynamic_pointer_cast<FRecordTestAnnotation>(Annotation))
			{
				SupplementalValue = Value->Value;
				++AnnotationCompletions;
			}
			PostEditUndo();
		}
		std::vector<std::string>* CaptureTrace = nullptr;
		mutable uint32 AnnotationCaptures = 0;
		size_t AnnotationBytes = sizeof(FRecordTestAnnotation);
		bool bAnnotationEnabled = false, bCollectDuringAnnotation = false, bReportAnnotationChanges = false;
		int32 SupplementalValue = 0, AnnotationCompletions = 0;
		Durin::DObject* AnnotationReference = nullptr;
		int32 UndoBegins = 0;
		int32 UndoCompletions = 0;
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
		std::vector<FRecordNestedValue> NestedArray;
		FRecordStructMap NestedMap;
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
			for (const Durin::Editor::FTransactionObjectRecord& Snapshot : Records)
				Snapshot.AddReferencedObjects(Collector);
		}

		std::vector<Durin::Editor::FTransactionObjectRecord> Records;
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
		Buffer.SetPrimaryUndoObject(Context.PrimaryObject);
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
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override
		{ return {}; }

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
	class FDeclaredParticipantChange final : public Durin::FCommandChange
	{
	public:
		FDeclaredParticipantChange(DTransactionRecordParticipant* InFirst,
			DTransactionRecordParticipant* InSecond, Durin::DObject* InDependency)
			: First(InFirst), Second(InSecond), Dependency(InDependency) {}
		auto GetDescription() const -> std::string_view override { return "Declared composite participants"; }
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override
		{ return {First, Second, First, nullptr}; }
		auto Apply() -> Durin::FCommandChangeResult override
		{ First->Value += 3; Second->Value += 5; return {}; }
		auto Revert() -> Durin::FCommandChangeResult override
		{ First->Value -= 3; Second->Value -= 5; return {}; }
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) const -> void override
		{ Dependency.AddReferencedObjects(Collector); }
	private:
		DTransactionRecordParticipant* First;
		DTransactionRecordParticipant* Second;
		Durin::FPersistentObjectRef Dependency;
	};

	class FDetachedChildChange final : public Durin::Editor::FActorCommandChange
	{
	public:
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override
		{ return {Owner, Child}; }

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

	auto CaptureRecord(DTransactionRecordParticipant* Target) -> Durin::Editor::FTransactionObjectRecord
	{
		auto Result = Durin::Editor::FTransactionObjectRecord::CaptureWhole(*Target);
		EXPECT_TRUE(Result) << (Result ? "" : Durin::Editor::FormatTransactionObjectRecordError(Result.error()));
		return Result ? std::move(*Result) : Durin::Editor::FTransactionObjectRecord{};
	}

}

TEST(FPersistentObjectRefTests, PreservesExactIdentityAndRejectsGarbageAndSlotReuse)
{
	InitializeDObjectSystem();
	auto* FirstOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PersistentFirstOuter"));
	auto* SecondOuter = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("PersistentSecondOuter"));
	auto* Object = Durin::NewObject<Durin::DObject>(FirstOuter, Durin::FName("PersistentObject"));
	const Durin::FPersistentObjectRef Reference(Object);
	EXPECT_EQ(Durin::FPersistentObjectRef(), Durin::FPersistentObjectRef(nullptr));
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

static_assert(std::is_abstract_v<Durin::DTransactor>);
static_assert(!std::is_abstract_v<Durin::DTransBuffer>);
static_assert(!std::is_copy_constructible_v<Durin::Editor::FTransaction>);
static_assert(std::is_move_constructible_v<Durin::Editor::FTransaction>);
static_assert(!std::is_copy_constructible_v<Durin::Editor::FScopedTransaction>);
static_assert(std::is_move_constructible_v<Durin::Editor::FScopedTransaction>);

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

TEST(FTransactionObjectEventTests, SynchronousEventsKeepContextAndRejectRecordingReentry)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SynchronousObjectEvents"));
	std::vector<FTransactionObjectEvent> Events; std::vector<std::string> Trace;
	Target->Trace = [&](std::string_view Value) { Trace.emplace_back(Value); };
	ITransaction* Recording = nullptr;
	Target->TransactionObserver = [&](const FTransactionObjectEvent& Event) {
		Events.push_back(Event); Trace.push_back("event");
		EXPECT_NE(Event.OperationId, 0u); EXPECT_NE(Event.TransactionId, 0u);
		if (Event.Type == ETransactionObjectEventType::Finalized)
		{
			EXPECT_EQ(GUndo, Recording); EXPECT_EQ(GUndo->GetContext().OperationId, Event.OperationId);
			EXPECT_EQ(Buffer->Begin("reentry", "reentry"), -1); EXPECT_EQ(Buffer->End(), 0);
			Buffer->Cancel(); EXPECT_EQ(GUndo, Recording);
			const auto Saves = Target->SerializationSaves; (void)Target->Modify(); EXPECT_EQ(Target->SerializationSaves, Saves);
			EXPECT_FALSE(Recording->EndOperation()); EXPECT_FALSE(Recording->Finalize());
			EXPECT_FALSE(Recording->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 0, 99)));
			EXPECT_EQ(Buffer->GetQueueLength(), 0u);
		}
	};
	ASSERT_EQ(Buffer->Begin("test", "Events"), 0); Recording = GUndo;
	ASSERT_TRUE(Target->Modify()); Target->Value = 4; Target->NativeValue = 9;
	ASSERT_EQ(Buffer->Begin("nested", "Nested"), 1); EXPECT_EQ(Buffer->End(), 2); EXPECT_TRUE(Events.empty());
	EXPECT_EQ(Buffer->End(), 1); EXPECT_EQ(GUndo, nullptr); ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events[0].Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Events[0].Change.ChangedProperties, std::vector<FName>{"Value"}); EXPECT_TRUE(Events[0].Change.bNonPropertyDataChanged);
	Trace.clear(); ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Trace, (std::vector<std::string>{"pre", "post", "event"}));
	ASSERT_EQ(Events.size(), 2u); EXPECT_EQ(Events[1].Operation, ETransactionOperation::Undo);
	EXPECT_NE(Events[0].OperationId, Events[1].OperationId);
	ASSERT_TRUE(Buffer->Redo()); ASSERT_EQ(Events.size(), 3u);
	EXPECT_EQ(Events[2].Operation, ETransactionOperation::Redo); EXPECT_NE(Events[1].OperationId, Events[2].OperationId);
	EXPECT_EQ(Events[0].TransactionId, Events[2].TransactionId);
}

TEST(FTransactionObjectEventTests, CancelNoOpAndCustomOnlyDoNotInventObjectEvents)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SilentObjectEvents"));
	uint32 Events = 0; Target->TransactionObserver = [&](const auto&) { ++Events; };
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "No op"}); ASSERT_TRUE(Target->Modify()); }
	EXPECT_EQ(Events, 0u); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Cancel"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; Scope.Cancel(); }
	EXPECT_EQ(Events, 0u);
	ASSERT_EQ(Buffer->Begin("test", "Custom observation"), 0);
	Target->Value = 2; ASSERT_TRUE(Buffer->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 1, 2)));
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); EXPECT_EQ(Events, 0u); EXPECT_EQ(Buffer->End(), 1);
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Events, 0u); EXPECT_EQ(Target->UndoCompletions, 2);
}

TEST(FTransactionObjectEventTests, CompensatedFailureEmitsNoSuccessfulReplayEvent)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CompensatedObjectEvents"));
	uint32 Events = 0; Target->TransactionObserver = [&](const auto&) { ++Events; };
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Failure"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	ASSERT_EQ(Events, 1u); Target->bFailLoadOnce = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Events, 1u); EXPECT_EQ(Target->Value, 4); EXPECT_TRUE(Buffer->CanUndo());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Events, 2u);
}

TEST(FTransactionObjectEventTests, ObserverExceptionRetainsRecordingAndRequiresRecovery)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingFinalizedEvent"));
	Target->TransactionObserver = [](const auto&) { throw std::runtime_error("Injected object event failure."); };
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Observer failure"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired); EXPECT_EQ(Buffer->GetLastResult().Code, ETransactorResultCode::RecoveryRequired);
	EXPECT_FALSE(Buffer->CanTransact()); EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_EQ(GUndo, nullptr); EXPECT_EQ(Target->Value, 4);
	TWeakObjectPtr<DTransactionRecordParticipant> Retained(Target.Get()); Target.Reset(); CollectGarbage(); EXPECT_TRUE(Retained.IsValid());
}

TEST(FTransactionObjectEventTests, ReplayObserverExceptionKeepsCursorAndCompletedNativeState)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingReplayEvent"));
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Replay observer"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	const auto Id = Buffer->GetUndoId();
	Target->TransactionObserver = [](const auto&) { throw std::runtime_error("Injected replay object event failure."); };
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_EQ(Buffer->GetUndoId(), Id); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->UndoCompletions, 1);
}

TEST(FTransactionObjectEventTests, SnapshotsAccumulateSelectionWithoutReplacingUndoBaseline)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SelectedSnapshots"));
	std::vector<FTransactionObjectEvent> Events;
	Target->TransactionObserver = [&](const auto& Event) {
		Events.push_back(Event); EXPECT_EQ(GUndo->GetContext().OperationId, Event.OperationId);
		EXPECT_FALSE(GUndo->SnapshotObject(Target.Get())); EXPECT_EQ(Buffer->Begin("reentry", "snapshot"), -1);
	};
	ASSERT_EQ(Buffer->Begin("test", "Snapshot selection"), 0);
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); EXPECT_TRUE(Events.empty()); ASSERT_TRUE(Target->Modify());
	const auto Operation = GUndo->GetContext().OperationId;
	const std::array<const FProperty*, 1> Value{Property("Value")}, Session{Property("SessionValue")};
	Target->Value = 1; Target->SessionValue = 2;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Value)); ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events.back().Change.ChangedProperties, std::vector<FName>{"Value"});
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Session)); EXPECT_EQ(Events.size(), 1u);
	Target->Value = 3; Target->SessionValue = 4;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Session)); ASSERT_EQ(Events.size(), 2u);
	EXPECT_TRUE(std::ranges::contains(Events.back().Change.ChangedProperties, FName("Value")));
	EXPECT_TRUE(std::ranges::contains(Events.back().Change.ChangedProperties, FName("SessionValue")));
	Target->Numbers = {1}; ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); ASSERT_EQ(Events.size(), 3u);
	Target->Numbers = {2}; ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Value)); ASSERT_EQ(Events.size(), 4u);
	EXPECT_EQ(Events.back().Change.ChangedProperties, std::vector<FName>{"Numbers"});
	EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_EQ(GUndo->GetContext().OperationId, Operation);
	Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	EXPECT_EQ(Buffer->End(), 1); ASSERT_EQ(Events.size(), 5u); EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SessionValue, 0); EXPECT_TRUE(Target->Numbers.empty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 3); EXPECT_EQ(Target->SessionValue, 4); EXPECT_EQ(Target->Numbers, std::vector<int32>{2});
}

TEST(FTransactionObjectEventTests, WholeObjectSnapshotsObserveNativeDataAndKeepOriginalUndo)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "WholeSnapshots"));
	std::vector<FTransactionObjectEvent> Events; Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Whole snapshot"), 0);
	auto Record = FTransactionObjectRecord::CaptureWhole(*Target);
	ASSERT_TRUE(Record); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Record)));
	Target->Value = 2; Target->NativeValue = 8;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); ASSERT_EQ(Events.size(), 1u);
	EXPECT_GT(Target->SerializationSaves, 0); EXPECT_TRUE(Events.back().Change.bNonPropertyDataChanged);
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); EXPECT_EQ(Events.size(), 1u);
	ASSERT_TRUE(Target->Modify()); Target->Value = 4; Target->NativeValue = 9;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Buffer->End(), 1); ASSERT_EQ(Events.size(), 3u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->NativeValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 4); EXPECT_EQ(Target->NativeValue, 9);
}

TEST(FTransactionObjectEventTests, SnapshotObserverFailureClosesRecordingAndRetainsRecoveryGraph)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingSnapshotEvent"));
	ASSERT_EQ(Buffer->Begin("test", "Snapshot failure"), 0); ASSERT_TRUE(Target->Modify()); Target->Value = 1;
	Target->TransactionObserver = [](const auto&) { throw std::runtime_error("Injected snapshot observer failure."); };
	EXPECT_FALSE(GUndo->SnapshotObject(Target.Get())); EXPECT_EQ(GUndo, nullptr);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired); EXPECT_FALSE(Buffer->IsActive());
	TWeakObjectPtr<DTransactionRecordParticipant> Retained(Target.Get()); Target.Reset(); CollectGarbage(); EXPECT_TRUE(Retained.IsValid());
}

TEST(FTransactionObjectEventTests, MultipleTargetsCompleteBeforeRecordAndAnnotationEvents)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationEventOwner"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationEventPeer"));
	First->bAnnotationEnabled = First->bReportAnnotationChanges = true; First->AnnotationReference = Second.Get();
	std::vector<std::string> Trace; std::vector<FTransactionObjectEvent> PeerEvents;
	First->Trace = [&](std::string_view Phase) { Trace.push_back(std::string(Phase) + "1"); };
	Second->Trace = [&](std::string_view Phase) { Trace.push_back(std::string(Phase) + "2"); };
	First->TransactionObserver = [&](const auto&) { Trace.push_back("event1"); };
	Second->TransactionObserver = [&](const auto& Event) { Trace.push_back("event2"); PeerEvents.push_back(Event); };
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Record and annotation events"}); ASSERT_TRUE(First->Modify()); ASSERT_TRUE(Second->Modify()); First->SupplementalValue = 3; Second->Value = 7; }
	ASSERT_EQ(PeerEvents.size(), 2u); EXPECT_EQ(Trace, (std::vector<std::string>{"event1", "event2", "event2"}));
	EXPECT_TRUE(PeerEvents[0].Change.bNonPropertyDataChanged); EXPECT_EQ(PeerEvents[0].CreatedBy, ETransactionObjectChangeCreatedBy::TransactionAnnotation);
	EXPECT_FALSE(PeerEvents.back().Change.bNonPropertyDataChanged); EXPECT_EQ(PeerEvents.back().Change.ChangedProperties, std::vector<FName>{"Value"});
	EXPECT_EQ(PeerEvents.back().CreatedBy, ETransactionObjectChangeCreatedBy::TransactionRecord);
	EXPECT_FALSE(PeerEvents.back().Annotation); EXPECT_EQ(PeerEvents.back().Change.OriginalName, Second->GetFName());
	Trace.clear(); ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Trace, (std::vector<std::string>{"pre1", "pre2", "post1", "post2", "event1", "event2", "event2"}));
	ASSERT_EQ(PeerEvents.size(), 4u); EXPECT_TRUE(PeerEvents[2].Change.bNonPropertyDataChanged);
	EXPECT_EQ(First->SupplementalValue, 0); EXPECT_EQ(Second->Value, 0);
}

TEST(FTransactionObjectEventTests, FinalizedAnnotationComparesTheLastSnapshotAfterReturningToOriginal)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Owner(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationSnapshotOwner"));
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationSnapshotPeer"));
	Owner->bAnnotationEnabled = Owner->bReportAnnotationChanges = true; Owner->AnnotationReference = Peer.Get();
	std::vector<FTransactionObjectEvent> Events; Peer->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Annotation snapshot closure"), 0); ASSERT_TRUE(Owner->Modify());
	Owner->SupplementalValue = 3; ASSERT_TRUE(GUndo->SnapshotObject(Owner.Get())); ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Snapshot);
	Owner->SupplementalValue = 0; EXPECT_EQ(Buffer->End(), 1); ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events.back().Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Events.back().CreatedBy, ETransactionObjectChangeCreatedBy::TransactionAnnotation);
	EXPECT_TRUE(Events.back().Change.bNonPropertyDataChanged); EXPECT_FALSE(Events.back().Annotation);
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

TEST(FTransBufferTests, RepeatedCapturePreservesFirstValuesAndLaterLiveCapture)
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

TEST(FTransBufferTests, AnnotationOnlyHistoryRestoresDesiredSideAndFlipsLiveSupplementalState)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationOnlyHistory"));
	Target->bAnnotationEnabled = true;
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Annotation only"}); ASSERT_TRUE(Target->Modify()); Target->SupplementalValue = 9; }
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	Target->SupplementalValue = 17;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->SupplementalValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->SupplementalValue, 17);
	EXPECT_EQ(Target->AnnotationCompletions, 2); EXPECT_EQ(Target->UndoBegins, 2); EXPECT_EQ(Target->UndoCompletions, 2);
}

TEST(FTransBufferTests, AnnotationReferencesSurviveCallbackCollectionAndReleaseWithHistory)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotationHistoryReferences"));
	Target->bAnnotationEnabled = true; Target->bCollectDuringAnnotation = true;
	Target->AnnotationReference = NewObject<DObject>(nullptr, "AnnotationRetainedReference");
	const auto Key = FObjectKey(Target->AnnotationReference);
	{ FScopedTransaction Scope(Buffer.Get(), {"test", "Annotation GC"}); ASSERT_TRUE(Target->Modify()); Target->SupplementalValue = 3; }
	Target->AnnotationReference = nullptr; CollectGarbage(); EXPECT_NE(ResolveObjectKey(Key), nullptr);
	ASSERT_TRUE(Buffer->Undo()); CollectGarbage(); EXPECT_NE(ResolveObjectKey(Key), nullptr);
	ASSERT_TRUE(Buffer->Reset()); CollectGarbage(); EXPECT_EQ(ResolveObjectKey(Key), nullptr);
}

TEST(FTransBufferTests, CompensatedLoadCompletesWithPreOperationAnnotation)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CompensatedAnnotation"));
	Target->bAnnotationEnabled = true; Target->SupplementalValue = 1;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Annotation compensation"}); ASSERT_TRUE(Target->Modify());
		Target->Value = 7; Target->SupplementalValue = 2;
	}
	Target->bFailLoadOnce = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Target->SupplementalValue, 2); EXPECT_EQ(Target->Value, 7);
	EXPECT_EQ(Target->AnnotationCompletions, 1); EXPECT_EQ(Buffer->GetUndoCount(), 0u); EXPECT_TRUE(Buffer->CanUndo());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->SupplementalValue, 1); EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->SupplementalValue, 2); EXPECT_EQ(Target->Value, 7);
	EXPECT_EQ(Target->AnnotationCompletions, 3); EXPECT_EQ(Target->UndoBegins, 3); EXPECT_EQ(Target->UndoCompletions, 3);
}

TEST(FTransBufferTests, LaterRecordFailurePreservesOriginalAnnotationForRetry)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotatedAtomicRetry"));
	Target->bAnnotationEnabled = true;
	int OpaqueValue = 0;
	auto Failure = std::make_unique<FTestCustomChange>(OpaqueValue, 0, 1); auto* Fault = Failure.get();
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Annotated atomic retry"});
		ASSERT_TRUE(ApplyEditorChange(*Buffer, std::move(Failure)));
		ASSERT_TRUE(Target->Modify()); Target->Value = 7; Target->SupplementalValue = 3;
	}
	Target->SupplementalValue = 11; Fault->bFailUndo = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Target->Value, 7); EXPECT_EQ(Target->SupplementalValue, 11);
	EXPECT_EQ(Buffer->GetState(), ETransactorState::Idle); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	Fault->bFailUndo = false;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SupplementalValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 7); EXPECT_EQ(Target->SupplementalValue, 11);
}

TEST(FTransBufferTests, WholeAnnotationKeepsFirstStateThroughRepeatedCapture)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "WholeAnnotation"));
	Target->bAnnotationEnabled = true; Target->SupplementalValue = 1;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Whole annotation capture"});
		auto Initial = FTransactionObjectRecord::CaptureWhole(*Target);
		ASSERT_TRUE(Initial); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Initial)));
		Target->Value = 2; Target->SupplementalValue = 7;
		auto Latest = FTransactionObjectRecord::CaptureWhole(*Target);
		ASSERT_TRUE(Latest); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Latest)));
		ASSERT_TRUE(Target->Modify());
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SupplementalValue, 1);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 2); EXPECT_EQ(Target->SupplementalValue, 7);
}

TEST(FEngineTransactionAnnotationTests, WholeRecordRestoresNativeRegistration)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("FocusedNativeActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	auto* Member = Root->GetClass()->FindPropertyByName("CreationMethod"); ASSERT_NE(Member, nullptr);
	FPropertyValueSnapshotPayload Before; ASSERT_TRUE(CapturePropertyValuePayload(Member, Root, 0, Before, true));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Focused native record"});
		auto Record = FTransactionObjectRecord::CaptureWhole(*Root);
		ASSERT_TRUE(Record); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Record)));
		Root->UnregisterComponent();
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_TRUE(Root->IsRegistered()); EXPECT_EQ(Root->Registered, 2u);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_FALSE(Root->IsRegistered()); EXPECT_EQ(Root->Registered, 2u);
}

TEST(FEngineTransactionAnnotationTests, DefaultComponentAnnotationIsImplicitAndRestoresDefaultNativeState)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DSceneComponent> Component(NewObject<DSceneComponent>(nullptr, "DefaultComponentAnnotation"));
	Component->SetFlags(EObjectFlags::Transactional);
	EXPECT_FALSE(Component->FindOrCreateTransactionAnnotation());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Default native snapshot"}); ASSERT_TRUE(Component->Modify());
		Component->SetComponentTickEnabled(true);
	}
	ASSERT_TRUE(Component->FindOrCreateTransactionAnnotation());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_FALSE(Component->IsComponentTickEnabled()); EXPECT_FALSE(Component->FindOrCreateTransactionAnnotation());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Component->IsComponentTickEnabled());
}

TEST(FEngineTransactionAnnotationTests, ExplicitAnnotationArchiveRoundTripsNativeStateWithoutDuplicatingCreation)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("AnnotationArchiveActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	FActorTransactionAnnotation Original(*Actor);
	FByteBuffer Bytes; FObjectMemoryWriter Writer(Bytes, EArchivePurpose::Transaction); Original.Serialize(Writer);
	ASSERT_FALSE(Writer.IsError()) << Writer.GetError();
	Actor->SetActorTickEnabled(true); Root->UnregisterComponent();
	FActorTransactionAnnotation Restored; FObjectMemoryReader Reader(Bytes, EArchivePurpose::Transaction); Restored.Serialize(Reader);
	ASSERT_FALSE(Reader.IsError()) << Reader.GetError(); EXPECT_EQ(Reader.GetRemainingPayloadBytes(), 0u);
	ASSERT_TRUE(Restored.Restore(*Actor)); EXPECT_FALSE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Root->IsRegistered());
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u);
}

TEST(FEngineTransactionAnnotationTests, ActorAndRootRestoreNativeLifecycleOncePerOperation)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("NativeAnnotationActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent()); ASSERT_NE(Root, nullptr);
	ASSERT_EQ(Root->Created, 1u); ASSERT_EQ(Root->Registered, 1u);
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Native annotation lifecycle"}); ASSERT_TRUE(Actor->Modify());
		Actor->SetActorTickEnabled(true); Root->SetComponentTickEnabled(true); Root->UnregisterComponent();
	}
	const auto Constructions = Actor->ConstructionCalls;
	Root->bCollectOnRegister = true;
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_FALSE(Actor->IsActorTickEnabled()); EXPECT_FALSE(Root->IsComponentTickEnabled()); EXPECT_TRUE(Root->IsRegistered());
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u); EXPECT_EQ(Root->Initialized, 1u);
	EXPECT_EQ(Actor->ConstructionCalls, Constructions + 1);
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_TRUE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Root->IsComponentTickEnabled()); EXPECT_FALSE(Root->IsRegistered());
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u); EXPECT_EQ(Root->Unregistered, 2u); EXPECT_EQ(Root->Uninitialized, 0u);
	EXPECT_EQ(Actor->ConstructionCalls, Constructions + 2);
}

TEST(FEngineTransactionAnnotationTests, GeneratedRootRecreatesWithoutRetainingOldGenerationAndRestoresExternalAttachment)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedRootActor"); ASSERT_NE(Actor, nullptr);
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachedActor"); ASSERT_NE(Other, nullptr);
	auto* Generated = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Generated, nullptr); ASSERT_TRUE(Actor->SetRootComponent(Generated));
	Generated->SetRelativeLocation({5.0, 0.0, 0.0});
	ASSERT_TRUE(Other->GetRootComponent()->AttachToComponent(Generated, EAttachmentTransformRule::KeepRelative));
	Other->GetRootComponent()->SetRelativeLocation({2.0, 0.0, 0.0});
	const auto BeforeWorld = Generated->GetWorldLocation(); const auto ChildWorld = Other->GetRootComponent()->GetWorldLocation();
	const auto OldKey = FObjectKey(Generated);
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Generated root state"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	EXPECT_EQ(Actor->GetRootComponent(), nullptr); CollectGarbage(); EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	Actor->bCollectDuringConstruction = true;
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	auto* Restored = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent()); ASSERT_NE(Restored, nullptr);
	EXPECT_NE(FObjectKey(Restored), OldKey); EXPECT_EQ(Restored->GetWorldLocation(), BeforeWorld); EXPECT_EQ(Restored->CreatedLocation, FVector3(0.0));
	EXPECT_EQ(Restored->Created, 1u); EXPECT_EQ(Restored->Registered, 1u); EXPECT_EQ(Restored->Initialized, 1u);
	EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), Restored); EXPECT_EQ(Other->GetRootComponent()->GetWorldLocation(), ChildWorld);
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_EQ(Actor->GetRootComponent(), nullptr);
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), Actor->GetRootComponent());
	EXPECT_EQ(Other->GetRootComponent()->GetWorldLocation(), ChildWorld);
}

TEST(FEngineTransactionAnnotationTests, SerializedChildEditsAndGeneratedRootReconstructionPreserveBothPoses)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("MixedRootActor");
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("MixedRootChild");
	ASSERT_NE(Actor, nullptr); ASSERT_NE(Other, nullptr);
	auto* Root = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_TRUE(Actor->SetRootComponent(Root)); Root->SetRelativeLocation({5.0, 0.0, 0.0});
	auto* Child = Other->GetRootComponent(); ASSERT_TRUE(Child->AttachToComponent(Root)); Child->SetRelativeLocation({2.0, 0.0, 0.0});
	const auto Before = Child->GetWorldLocation();
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Serialized child and root reconstruction"});
		ASSERT_TRUE(Actor->Modify()); ASSERT_TRUE(Child->Modify()); Child->SetRelativeLocation({3.0, 0.0, 0.0});
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	const auto After = Child->GetWorldLocation(); EXPECT_EQ(After, FVector3(8.0, 0.0, 0.0));
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult()); EXPECT_EQ(Child->GetWorldLocation(), Before);
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult()); EXPECT_EQ(Child->GetWorldLocation(), After);
}

TEST(FEngineTransactionAnnotationTests, SwitchingGeneratedAndAuthoredRootsPreservesBothHistorySides)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("RootKindSwitchActor"); ASSERT_NE(Actor, nullptr);
	auto* NativeRoot = Actor->GetRootComponent();
	auto* GeneratedRoot = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(GeneratedRoot, nullptr); ASSERT_TRUE(Actor->SetRootComponent(GeneratedRoot));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Root kind switch"}); ASSERT_TRUE(Actor->Modify());
		ASSERT_TRUE(Actor->SetRootComponent(NativeRoot));
	}
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult()); EXPECT_EQ(Actor->GetRootComponent(), Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	EXPECT_NE(Actor->GetRootComponent(), GeneratedRoot); EXPECT_TRUE(GeneratedRoot->IsPendingKill());
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult()); EXPECT_EQ(Actor->GetRootComponent(), NativeRoot);
}

TEST(FEngineTransactionAnnotationTests, FailedReconstructionCleansNewOutputAndRequiresRecovery)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("FailedReconstructionActor"); ASSERT_NE(Actor, nullptr);
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Reconstruction failure"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 2; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	const auto Before = Actor->GetComponentsSnapshot(); Actor->bFailConstruction = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_EQ(Actor->GetComponentsSnapshot(), Actor->GetAuthoredComponents());
	for (const auto& Component : Before)
		if (Component->GetCreationMethod() == EComponentCreationMethod::Generated) EXPECT_TRUE(Component->IsPendingKill());
	Actor->bFailConstruction = false;
	ASSERT_TRUE(Actor->RerunConstructionScripts()); // Failure must release the native construction guard.
	EXPECT_EQ(Actor->GetComponents().size(), 2u);
}

TEST(FEngineTransactionAnnotationTests, ExpiredAnnotationParticipantRejectsBeforeSerializedMutation)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("AnnotationExpiryActor");
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("AnnotationExpiryChild");
	ASSERT_NE(Actor, nullptr); ASSERT_NE(Other, nullptr);
	auto* Generated = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_TRUE(Actor->SetRootComponent(Generated)); ASSERT_TRUE(Other->GetRootComponent()->AttachToComponent(Generated));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Expired annotation participant"}); ASSERT_TRUE(Actor->Modify());
		Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	MarkAsGarbage(Other->GetRootComponent());
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Actor->GeneratedCount, 0); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_NE(Buffer->GetState(), ETransactorState::RecoveryRequired);
}

TEST(FEngineTransactionAnnotationTests, RetirementInsideRegistrationCallbackRequiresRecoveryWithoutReinitializingGarbage)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("CallbackRetirementActor"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Retiring callback"}); ASSERT_TRUE(Actor->Modify()); Root->UnregisterComponent();
	}
	const auto Initializations = Root->Initialized; Root->bDestroyOnRegister = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	EXPECT_TRUE(Root->IsGarbage()); EXPECT_EQ(Root->Initialized, Initializations); EXPECT_EQ(Buffer->GetUndoCount(), 0u);
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

TEST(FTransBufferTests, ExplicitRecordsAndModifyShareFirstBeforeAndFinalAfter)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SharedWholeRecord"));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Mixed object recording"});
		auto Record = FTransactionObjectRecord::CaptureWhole(*Target);
		ASSERT_TRUE(Record); ASSERT_TRUE(FTransBufferTestAccess::Record(*Buffer, std::move(*Record)));
		Target->Value = 1;
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

TEST(FTransactionObjectEventTests, FinalizedClosesLastSnapshotEvenWhenHistoryReturnsToNoOp)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SnapshotClosureNoOp"));
	std::vector<FTransactionObjectEvent> Events;
	Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Snapshot closure"), 0); ASSERT_TRUE(Target->Modify());
	const auto Context = GUndo->GetContext();
	Target->Value = 4; Target->NativeValue = 9;
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get())); ASSERT_EQ(Events.size(), 1u);
	Target->Value = 0; Target->NativeValue = 0;
	EXPECT_EQ(Buffer->End(), 1); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	ASSERT_EQ(Events.size(), 2u);
	const auto& Final = Events.back();
	EXPECT_EQ(Final.Type, ETransactionObjectEventType::Finalized);
	EXPECT_EQ(Final.CreatedBy, ETransactionObjectChangeCreatedBy::TransactionRecord);
	EXPECT_EQ(Final.TransactionId, Context.TransactionId); EXPECT_EQ(Final.OperationId, Context.OperationId);
	EXPECT_EQ(Final.Change.OriginalObject, FObjectKey(Target.Get()));
	EXPECT_EQ(Final.Change.ChangedProperties, (std::vector<FName>{"Value"}));
	EXPECT_TRUE(Final.Change.bNonPropertyDataChanged);
}

TEST(FTransactionObjectEventTests, SelectedSnapshotClosureDoesNotReportUnobservedNetZeroFields)
{
	using namespace Durin; using namespace Durin::Editor;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SelectedSnapshotClosure"));
	std::vector<FTransactionObjectEvent> Events;
	Target->TransactionObserver = [&](const auto& Event) { Events.push_back(Event); };
	ASSERT_EQ(Buffer->Begin("test", "Selected closure"), 0); ASSERT_TRUE(Target->Modify());
	Target->Value = 4; Target->SessionValue = 9;
	const std::array<const FProperty*, 1> Selected{Property("Value")};
	ASSERT_TRUE(GUndo->SnapshotObject(Target.Get(), Selected)); ASSERT_EQ(Events.size(), 1u);
	Target->Value = 0; Target->SessionValue = 0;
	EXPECT_EQ(Buffer->End(), 1); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	ASSERT_EQ(Events.size(), 2u);
	EXPECT_EQ(Events.back().Change.ChangedProperties, (std::vector<FName>{"Value"}));
	EXPECT_FALSE(Events.back().Change.bNonPropertyDataChanged);
}

TEST(FEngineTransactionAnnotationTests, OrdinaryReconstructionConsumesTheSameGeneratedRootCache)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("OrdinaryCachedRoot");
	auto* Other = Fixture.World->SpawnActor<DTransactionConstructionActor>("OrdinaryCachedChild");
	ASSERT_NE(Actor, nullptr); ASSERT_NE(Other, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_TRUE(Actor->SetRootComponent(Root)); Root->SetRelativeLocation({5.0, 0.0, 0.0});
	ASSERT_TRUE(Other->GetRootComponent()->AttachToComponent(Root)); Other->GetRootComponent()->SetRelativeLocation({2.0, 0.0, 0.0});
	const auto RootPose = Root->GetWorldLocation(), ChildPose = Other->GetRootComponent()->GetWorldLocation();
	const auto OldKey = FObjectKey(Root);
	FComponentInstanceDataCache InstanceData(*Actor);
	FActorRootComponentReconstructionData RootData(*Actor);
	Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	CollectGarbage(); EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	Actor->GeneratedCount = 1; Actor->bCollectDuringConstruction = true;
	ASSERT_TRUE(Actor->RerunConstructionScripts(&InstanceData, &RootData));
	auto* Restored = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent()); ASSERT_NE(Restored, nullptr);
	EXPECT_NE(FObjectKey(Restored), OldKey); EXPECT_EQ(Restored->GetWorldLocation(), RootPose);
	EXPECT_EQ(Restored->CreatedLocation, FVector3(0.0)); EXPECT_EQ(Restored->Created, 1u); EXPECT_EQ(Restored->Registered, 1u);
	EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), Restored); EXPECT_EQ(Other->GetRootComponent()->GetWorldLocation(), ChildPose);
}

TEST(FEngineTransactionAnnotationTests, GeneratedSceneOverridesReplayWithoutAnAuthoredPropertyRecord)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("GeneratedPoseReplay"); ASSERT_NE(Actor, nullptr);
	auto* Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); Scene->SetRelativeLocation({9.0, 0.0, 0.0});
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Generated scene pose"}); ASSERT_TRUE(Actor->Modify());
		Scene->SetRelativeLocation({12.0, 0.0, 0.0}); ASSERT_TRUE(Actor->RerunConstructionScripts());
	}
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), FVector3(9.0, 0.0, 0.0));
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), FVector3(12.0, 0.0, 0.0));
	Actor->ConstructionLocation = FVector3(12.0, 0.0, 0.0);
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), *Actor->ConstructionLocation);
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	Actor->ConstructionLocation = FVector3(4.0, 0.0, 0.0);
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); EXPECT_EQ(Scene->GetRelativeLocation(), *Actor->ConstructionLocation);
}

TEST(FEngineTransactionAnnotationTests, InstanceDataFollowsConstructionAndCreationCallbacks)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("CandidateInstancePose"); ASSERT_NE(Actor, nullptr);
	auto* Scene = static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Scene, nullptr); Scene->SetRelativeLocation({9.0, 0.0, 0.0});
	FComponentInstanceDataCache InstanceData(*Actor);
	Actor->GeneratedCount = 0; ASSERT_TRUE(Actor->RerunConstructionScripts());
	struct FResetNativePose { ~FResetNativePose() { DTransactionLifecycleScene::NativeInitialLocation = FVector3(0.0); } } Reset;
	DTransactionLifecycleScene::NativeInitialLocation = FVector3(2.0, 0.0, 0.0);
	Actor->GeneratedCount = 1; ASSERT_TRUE(Actor->RerunConstructionScripts(&InstanceData));
	auto* Restored = static_cast<DTransactionLifecycleScene*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	ASSERT_NE(Restored, nullptr);
	EXPECT_EQ(Restored->GetRelativeLocation(), FVector3(9.0, 0.0, 0.0));
	EXPECT_EQ(Restored->CreatedLocation, FVector3(0.0));
	EXPECT_EQ(Restored->Created, 1u); EXPECT_EQ(Restored->Registered, 1u);
}

TEST(FEngineTransactionAnnotationTests, EmptyActorAnnotationIsOmittedAndDefaultTickRestoresOnUndo)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<AActor>("SparseActorAnnotation"); ASSERT_NE(Actor, nullptr);
	ASSERT_TRUE(Actor->GetPrimaryActorTick().IsTickFunctionRegistered());
	EXPECT_FALSE(Actor->FindOrCreateTransactionAnnotation());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Actor tick state"}); ASSERT_TRUE(Actor->Modify());
		Actor->SetActorTickEnabled(true);
		Actor->GetPrimaryActorTick().UnregisterTickFunction();
	}
	EXPECT_TRUE(Actor->FindOrCreateTransactionAnnotation());
	ASSERT_TRUE(Buffer->Undo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_FALSE(Actor->IsActorTickEnabled()); EXPECT_TRUE(Actor->GetPrimaryActorTick().IsTickFunctionRegistered());
	ASSERT_TRUE(Buffer->Redo()) << FormatTransactorResult(Buffer->GetLastResult());
	EXPECT_TRUE(Actor->IsActorTickEnabled()); EXPECT_FALSE(Actor->GetPrimaryActorTick().IsTickFunctionRegistered());
}

TEST(FEngineTransactionAnnotationTests, OrdinaryReregistrationPreservesPlayInitializationTickAndTimers)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture(EWorldType::Game);
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("OrdinaryReregister"); ASSERT_NE(Actor, nullptr);
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	ASSERT_TRUE(Fixture.World->BeginPlay({}));
	ASSERT_TRUE(Root->SetComponentTickGroup(ETickingGroup::PostPhysics)); Root->SetComponentTickEnabled(true);
	const auto Before = Root->CaptureLifecycleState();
	int TimerCalls = 0;
	const auto Timer = Fixture.World->GetTimerManager().SetTimerForObjectNextTick(Root, [&](DObject&) { ++TimerCalls; });
	{
		FComponentReregisterContext Outer(Root);
		EXPECT_FALSE(Root->IsRegistered()); EXPECT_TRUE(Root->HasBeenInitialized()); EXPECT_TRUE(Root->HasBegunPlay());
		{ FComponentReregisterContext Nested(Root); EXPECT_FALSE(Root->IsRegistered()); }
		EXPECT_FALSE(Root->IsRegistered());
		ASSERT_TRUE(Outer.ReRegister()); ASSERT_TRUE(Outer.ReRegister());
	}
	EXPECT_EQ(Root->CaptureLifecycleState(), Before);
	EXPECT_EQ(Root->Created, 1u); EXPECT_EQ(Root->Registered, 2u); EXPECT_EQ(Root->Unregistered, 1u);
	EXPECT_EQ(Root->Initialized, 1u); EXPECT_EQ(Root->Uninitialized, 0u); EXPECT_EQ(Root->Begun, 1u); EXPECT_EQ(Root->Ended, 0u);
	EXPECT_TRUE(Fixture.World->GetTimerManager().IsTimerActive(Timer));
	Fixture.World->Tick({.DeltaSeconds = 0.1f}); EXPECT_EQ(TimerCalls, 1);
	Root->ReregisterComponent(); EXPECT_EQ(Root->Registered, 3u); EXPECT_EQ(Root->Initialized, 1u); EXPECT_EQ(Root->Begun, 1u);
	Fixture.World->EndPlay(); EXPECT_EQ(Root->Ended, 1u);
}

TEST(FEngineTransactionAnnotationTests, SharedLifecycleRestoresUnregisteredInitializedStateWithoutCreationDuplication)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("UnregisteredInitialized");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	Root->UnregisterComponent(); ASSERT_TRUE(Root->HasBeenInitialized());
	const auto Target = Root->CaptureLifecycleState();
	ASSERT_TRUE(Root->ApplyLifecycleState({}));
	EXPECT_FALSE(Root->HasBeenCreated()); EXPECT_FALSE(Root->HasBeenInitialized());
	ASSERT_TRUE(Root->ApplyLifecycleState(Target)); EXPECT_EQ(Root->CaptureLifecycleState(), Target);
	EXPECT_EQ(Root->Created, 2u); EXPECT_EQ(Root->Initialized, 2u);
	const auto Registered = Root->Registered;
	ASSERT_TRUE(Root->ApplyLifecycleState(Target)); EXPECT_EQ(Root->Registered, Registered);
}

TEST(FEngineTransactionAnnotationTests, ReregisterScopeDoesNotResurrectRetiredComponentAndDefersCollection)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("RetiredReregister");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	const FObjectKey Key(Root);
	{
		FComponentReregisterContext Context(Root);
		Root->DestroyComponent(); CollectGarbage();
		EXPECT_TRUE(Root->IsPendingKill()); EXPECT_FALSE(Root->IsRegistered());
		ASSERT_TRUE(Context.ReRegister()); EXPECT_EQ(Root->Registered, 1u);
	}
	CollectGarbage(); EXPECT_EQ(ResolveObjectKey(Key), nullptr);
}

TEST(FEngineTransactionAnnotationTests, ReregisterCallbackFailureIsReportedAndConsumedWithoutRetry)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("ThrowingReregister");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		FComponentReregisterContext Context(Root);
		Root->bThrowOnRegister = true;
		const auto Result = Context.ReRegister(); ASSERT_FALSE(Result);
		EXPECT_NE(Result.error().find("Injected registration"), std::string::npos);
		Root->bThrowOnRegister = false;
	}
	EXPECT_EQ(Root->Registered, 2u); // A partially executed callback is never retried implicitly.
	Root->UnregisterComponent(); Root->RegisterComponent(); EXPECT_TRUE(Root->IsRegistered());
}

TEST(FEngineTransactionAnnotationTests, InnerRegistrationFailureRequiresRecoveryAndReleasesActorSuspension)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("InnerCallbackFailure");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Inner registration failure"}); ASSERT_TRUE(Actor->Modify());
		Root->UnregisterComponent();
	}
	Root->bThrowOnRegister = true;
	EXPECT_FALSE(Buffer->Undo()); EXPECT_EQ(Buffer->GetState(), ETransactorState::RecoveryRequired);
	Root->bThrowOnRegister = false;
	const auto Before = Actor->ConstructionCalls;
	ASSERT_TRUE(Actor->RerunConstructionScripts()); EXPECT_EQ(Actor->ConstructionCalls, Before + 1);
}

TEST(FEngineTransactionAnnotationTests, ReregisterScopePreservesTickConfigurationEdits)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("ReregisterTickEdit");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	{
		FComponentReregisterContext Context(Root);
		ASSERT_TRUE(Root->SetComponentTickGroup(ETickingGroup::PostPhysics)); Root->SetComponentTickEnabled(true);
	}
	EXPECT_EQ(Root->GetPrimaryComponentTick().GetTickGroup(), ETickingGroup::PostPhysics);
	EXPECT_TRUE(Root->IsComponentTickEnabled()); EXPECT_TRUE(Root->GetPrimaryComponentTick().IsTickFunctionRegistered());
	EXPECT_EQ(Root->Initialized, 1u); EXPECT_EQ(Root->Registered, 2u);
}

TEST(FEngineTransactionAnnotationTests, AttachmentReverseLinksAreSettledBeforeHistoricalRegistrationCallback)
{
	using namespace Durin; using namespace Durin::Editor;
	FTransactionEngineWorld Fixture; Tests::FTestTransactorOwner Buffer;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackChild");
	auto* First = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackFirst");
	auto* Second = Fixture.World->SpawnActor<DTransactionConstructionActor>("AttachmentCallbackSecond");
	auto* Root = static_cast<DTransactionLifecycleScene*>(Actor->GetRootComponent());
	ASSERT_TRUE(Root->AttachToComponent(First->GetRootComponent()));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Attachment callback state"}); ASSERT_TRUE(Root->Modify());
		ASSERT_TRUE(Root->AttachToComponent(Second->GetRootComponent())); Root->UnregisterComponent();
	}
	bool Called = false;
	Root->RegistrationCallback = [&] {
		Called = true;
		EXPECT_EQ(Root->GetAttachParent(), First->GetRootComponent());
		EXPECT_EQ(std::ranges::count_if(First->GetRootComponent()->GetAttachChildren(), [&](const auto& Child) { return Child.Get() == Root; }), 1);
		EXPECT_EQ(std::ranges::count_if(Second->GetRootComponent()->GetAttachChildren(), [&](const auto& Child) { return Child.Get() == Root; }), 0);
	};
	ASSERT_TRUE(Buffer->Undo()); EXPECT_TRUE(Called); Root->RegistrationCallback = {};
}

TEST(FEngineTransactionAnnotationTests, IncompatibleActorAnnotationReleasesSharedSuspension)
{
	using namespace Durin;
	FTransactionEngineWorld Fixture;
	auto* Actor = Fixture.World->SpawnActor<DTransactionConstructionActor>("WrongAnnotationSuspension");
	Actor->PreEditUndo();
	EXPECT_THROW(Actor->PostEditUndo(std::make_shared<FComponentTransactionAnnotation>(*Actor->GetRootComponent())), std::runtime_error);
	const auto Before = Actor->ConstructionCalls;
	ASSERT_TRUE(Actor->RerunConstructionScripts()); EXPECT_EQ(Actor->ConstructionCalls, Before + 1);
}

namespace
{
	class FReinstancedValueCommand final : public Durin::FCommandChange
	{
	public:
		inline static std::function<void(Durin::DObject*)> DestructionObserver;
		~FReinstancedValueCommand() override { if (DestructionObserver) DestructionObserver(Target.Resolve()); }
		FReinstancedValueCommand(DTransactionRecordParticipant* Object, int32 InBefore, int32 InAfter, bool InSupport = true)
			: Target(Object), Before(InBefore), After(InAfter), bSupport(InSupport), Packages{Object->GetPackage()} {}
		auto Apply() -> Durin::FCommandChangeResult override { static_cast<DTransactionRecordParticipant*>(Target.Resolve())->Value = After; return {}; }
		auto Revert() -> Durin::FCommandChangeResult override { static_cast<DTransactionRecordParticipant*>(Target.Resolve())->Value = Before; return {}; }
		auto GetDescription() const -> std::string_view override { return "Reinstanced native value"; }
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override { return {Target.Resolve()}; }
		auto GetAffectedPackages() const -> std::span<Durin::DPackage* const> override { return Packages; }
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) const -> void override { Target.AddReferencedObjects(Collector); }
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		auto CreateReplacement(const Durin::FObjectReplacementMap& Map) const -> std::expected<std::unique_ptr<Durin::FCommandChange>, std::string> override
		{
			if (!bSupport) return Durin::FCommandChange::CreateReplacement(Map);
			auto Key = Map.RemapObjectKey(Target.GetKey());
			if (!Key) return std::unexpected(Durin::ToString(Key.error()));
			return std::make_unique<FReinstancedValueCommand>(static_cast<DTransactionRecordParticipant*>(Durin::ResolveObjectKey(*Key)), Before, After);
		}
	private:
		Durin::FPersistentObjectRef Target;
		int32 Before, After;
		bool bSupport;
		std::array<Durin::DPackage*, 1> Packages;
	};

	class FTransactionReinstancingTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			using namespace Durin;
			InitializeDObjectSystem();
			Testing::RegisterMountPointForTests("/TransactionReinstance/", Testing::GetTestWorkDirectory().generic_string() + "/");
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/TransactionReinstance/History", Path));
			Current = CreatePackage(Path);
			Candidate = NewObject<DPackage>(nullptr, "ReinstancingCandidate");
			ASSERT_TRUE(Candidate->InitializePreparedAssetPackage(Path));
			Old = NewObject<DTransactionRecordParticipant>(Current, "Target", EObjectFlags::Public);
			New = NewObject<DTransactionRecordParticipant>(Candidate, "Target", EObjectFlags::Public);
			Buffer = TStrongObjectPtr<DTransBuffer>(NewObject<DTransBuffer>(nullptr, "ReinstancingHistory"));
			Buffer->EstablishSavedState(*Current);
			Pair = {Current, Candidate};
		}
		auto TearDown() -> void override
		{
			Operation.Abort();
			(void)Operation.Retire();
			Buffer.Reset();
			Durin::MarkObjectHierarchyAsGarbage(Current);
			Durin::MarkObjectHierarchyAsGarbage(Candidate);
			Durin::CollectGarbage();
		}
		auto Record(int32 Value) -> void
		{
			Durin::Editor::FScopedTransaction Scope(Buffer.Get(), {"test", "Reinstanced value", Old});
			ASSERT_TRUE(Old->Modify());
			Old->Value = Value;
			ASSERT_TRUE(Scope.End());
		}
		auto Prepare() -> std::expected<void, Durin::FObjectReplacementError>
		{
			const std::shared_ptr<Durin::IObjectReplacementParticipant> Participants[]{
				Durin::CreateTransactorReloadParticipant(*Buffer, std::span(&Current, 1))};
			return Operation.Prepare(std::span(&Pair, 1), Participants);
		}
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::DPackage* Current = nullptr;
		Durin::DPackage* Candidate = nullptr;
		DTransactionRecordParticipant* Old = nullptr;
		DTransactionRecordParticipant* New = nullptr;
		Durin::TStrongObjectPtr<Durin::DTransBuffer> Buffer;
		Durin::FObjectReplacementPackagePair Pair;
		Durin::FObjectGraphReplacement Operation;
	};
}

TEST_F(FTransactionReinstancingTests, RetainsUndoRedoPrimaryIdentityAndNewPackageCheckpoint)
{
	using namespace Durin;
	Record(42);
	const auto Id = Buffer->GetUndoId();
	New->Value = 77;
	const FObjectKey OldKey(Old);
	auto Ready = Prepare(); ASSERT_TRUE(Ready) << ToString(Ready.error());
	EXPECT_EQ(New->SerializationLoads, 0u);
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Old);
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoId(), Id);
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, New);
	EXPECT_FALSE(Buffer->GetPackageRevisionState(*Current).has_value());
	ASSERT_TRUE(Operation.Retire());
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 0);
	EXPECT_TRUE(Candidate->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 77);
	EXPECT_FALSE(Candidate->IsDirty());
	EXPECT_TRUE(Buffer->GetPackageRevisionState(*Candidate)->bCheckpointValid);
}

TEST_F(FTransactionReinstancingTests, RedoBranchSurvivesReinstancingAtUndonePosition)
{
	Record(42);
	ASSERT_TRUE(Buffer->Undo());
	New->Value = 9;
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_TRUE(Buffer->CanRedo()); EXPECT_FALSE(Buffer->CanUndo());
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 42);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 9);
	EXPECT_FALSE(Candidate->IsDirty());
}

TEST_F(FTransactionReinstancingTests, AbortAndPersistenceFailureLeaveOriginalHistoryUntouched)
{
	Record(12);
	ASSERT_TRUE(Prepare());
	EXPECT_FALSE(Operation.TryCommit([]() -> std::expected<void, Durin::FObjectReplacementError> {
		return std::unexpected(Durin::FObjectReplacementError{.Code = Durin::EObjectReplacementError::ParticipantRejected});
	}));
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Old);
	Operation.Abort();
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Old->Value, 0);
	EXPECT_EQ(New->SerializationLoads, 0u);
}

TEST_F(FTransactionReinstancingTests, ChangedHistoryAfterPrepareRejectsPublication)
{
	Record(12);
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_FALSE(Operation.TryCommit());
	EXPECT_EQ(Durin::FindPackage(Current->GetPackagePath()), Current);
	EXPECT_EQ(Buffer->GetRedoContext().PrimaryObject, Old);
}

TEST_F(FTransactionReinstancingTests, UnsupportedAnnotationRetiresWholeTransactionWithoutPartialRemap)
{
	Old->bAnnotationEnabled = true; Old->AnnotationReference = Old;
	Record(12);
	ASSERT_TRUE(Prepare());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_FALSE(Buffer->CanUndo());
	ASSERT_TRUE(Operation.Retire());
}

TEST_F(FTransactionReinstancingTests, ActorAnnotationRebindsTypedSceneCacheWithoutChangingOriginalSnapshot)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	ASSERT_TRUE(AfterActor->RerunConstructionScripts());
	auto* BeforeRoot = static_cast<DSceneComponent*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	auto* AfterRoot = static_cast<DSceneComponent*>(AfterActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	BeforeRoot->SetRelativeLocation({17.0, 0.0, 0.0});
	std::shared_ptr<ITransactionObjectAnnotation> Source = std::make_shared<FActorTransactionAnnotation>(*BeforeActor);
	FObjectReplacementMap Map;
	ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	auto Replaced = PrepareTransactionAnnotationReplacement(Source, Map);
	ASSERT_TRUE(Replaced) << Replaced.error();
	EXPECT_NE(Replaced->get(), Source.get());
	EXPECT_EQ(AfterRoot->GetRelativeLocation().x, 0.0);
	const auto* Annotation = dynamic_cast<const FActorTransactionAnnotation*>(Replaced->get());
	ASSERT_NE(Annotation, nullptr);
	ASSERT_TRUE(Annotation->GetComponentInstanceDataCache().ApplyToActor(*AfterActor, ECacheApplyPhase::PostUserConstructionScript));
	EXPECT_EQ(AfterRoot->GetRelativeLocation().x, 17.0);
	EXPECT_TRUE(Source->Validate());
	EXPECT_FALSE(Annotation->Restore(*BeforeActor));
}

TEST_F(FTransactionReinstancingTests, CustomCommandOptInRebindsFrozenParticipantsAndPackages)
{
	using namespace Durin;
	ASSERT_EQ(Buffer->Begin("test", "Native command"), 0);
	Old->Value = 12;
	ASSERT_TRUE(GUndo->StoreUndo(Old, std::make_unique<FReinstancedValueCommand>(Old, 0, 12)));
	ASSERT_EQ(Buffer->End(), 1);
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 12);
}

TEST_F(FTransactionReinstancingTests, UnsupportedCommandRetiresItsGenericPeerAsOneTransaction)
{
	using namespace Durin;
	FPackagePath OtherPath; ASSERT_TRUE(FPackagePath::TryCreate("/TransactionReinstance/Other", OtherPath));
	auto* OtherPackage = CreatePackage(OtherPath);
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(OtherPackage, "UnmappedCommandPeer"));
	Buffer->EstablishSavedState(*OtherPackage);
	ASSERT_EQ(Buffer->Begin("test", "Mixed native command"), 0);
	ASSERT_TRUE(Peer->Modify()); Peer->Value = 9;
	Old->Value = 12;
	ASSERT_TRUE(GUndo->StoreUndo(Old, std::make_unique<FReinstancedValueCommand>(Old, 0, 12, false)));
	ASSERT_EQ(Buffer->End(), 1);
	ASSERT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 2u);
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_EQ(Peer->Value, 9);
	const auto State = Buffer->GetPackageRevisionState(*OtherPackage);
	ASSERT_TRUE(State); EXPECT_FALSE(State->bCheckpointValid);
	EXPECT_TRUE(OtherPackage->IsDirty());
	ASSERT_TRUE(Operation.Retire());
	MarkObjectHierarchyAsGarbage(OtherPackage);
}

TEST_F(FTransactionReinstancingTests, CanceledRecordingAfterPreparationInvalidatesPreparedHistory)
{
	using namespace Durin;
	Record(12);
	(void)Tests::CollectTransactionEvents(*Buffer);
	ASSERT_TRUE(Prepare());
	ASSERT_EQ(Buffer->Begin("test", "Temporary recording"), 0);
	Buffer->Cancel();
	(void)Tests::CollectTransactionEvents(*Buffer);
	EXPECT_FALSE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Old);
}

TEST_F(FTransactionReinstancingTests, ComponentAnnotationRebindsExactTargetWithoutLifecycleCallbacks)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	auto* BeforeRoot = static_cast<DTransactionLifecycleScene*>(BeforeActor->GetRootComponent());
	auto* AfterRoot = static_cast<DTransactionLifecycleScene*>(AfterActor->GetRootComponent());
	std::shared_ptr<ITransactionObjectAnnotation> Source = std::make_shared<FComponentTransactionAnnotation>(*BeforeRoot);
	FObjectReplacementMap Map; ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	auto Replaced = PrepareTransactionAnnotationReplacement(Source, Map);
	ASSERT_TRUE(Replaced) << Replaced.error();
	const auto* Annotation = dynamic_cast<const FComponentTransactionAnnotation*>(Replaced->get());
	ASSERT_NE(Annotation, nullptr);
	EXPECT_EQ(Annotation->GetComponent(), AfterRoot);
	EXPECT_EQ(static_cast<const FComponentTransactionAnnotation*>(Source.get())->GetComponent(), BeforeRoot);
	EXPECT_EQ(AfterRoot->Created, 0u); EXPECT_EQ(AfterRoot->Registered, 0u);
}

TEST_F(FTransactionReinstancingTests, GeneratedSourceRejectsSameNamedAuthoredComponentAndPreservesCache)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	auto* BeforeScene = static_cast<DSceneComponent*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	BeforeScene->SetRelativeLocation({17.0, 0.0, 0.0});
	FComponentInstanceDataCache Cache(*BeforeActor, true);
	auto* Impostor = AfterActor->AddInstanceComponent(DTransactionLifecycleScene::StaticClass(), BeforeScene->GetFName());
	ASSERT_NE(Impostor, nullptr);
	FObjectReplacementMap Map; ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	EXPECT_FALSE(Cache.FindAndReplaceInstances(Map));
	EXPECT_TRUE(Cache.Validate(BeforeActor));
	EXPECT_FALSE(Cache.Validate(AfterActor));
}

TEST_F(FTransactionReinstancingTests, ActorHistoryReinstancingRestoresGeneratedInstanceDataAfterReconstruction)
{
	using namespace Durin; using namespace Durin::Editor;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor", EObjectFlags::Transactional);
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor", EObjectFlags::Transactional);
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	static_cast<DSceneComponent*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->SetRelativeLocation({17.0, 0.0, 0.0});
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Generated history", BeforeActor});
		ASSERT_TRUE(BeforeActor->Modify()); BeforeActor->GeneratedCount = 2;
		ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
		ASSERT_TRUE(Scope.End());
	}
	AfterActor->GeneratedCount = 2; ASSERT_TRUE(AfterActor->RerunConstructionScripts());
	static_cast<DSceneComponent*>(AfterActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->SetRelativeLocation({77.0, 0.0, 0.0});
	const auto ConstructionCalls = AfterActor->ConstructionCalls;
	auto Ready = Prepare(); ASSERT_TRUE(Ready) << ToString(Ready.error());
	EXPECT_EQ(AfterActor->ConstructionCalls, ConstructionCalls);
	ASSERT_TRUE(Operation.TryCommit()); ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(AfterActor->GeneratedCount, 1);
	EXPECT_EQ(static_cast<DSceneComponent*>(AfterActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->GetRelativeLocation().x, 17.0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(AfterActor->GeneratedCount, 2);
	EXPECT_EQ(static_cast<DSceneComponent*>(AfterActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->GetRelativeLocation().x, 77.0);
}

TEST_F(FTransactionReinstancingTests, DerivedInstanceDataWithoutTypedCopyRejectsWithoutSlicingOrChangingSource)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	ASSERT_TRUE(AfterActor->RerunConstructionScripts());
	auto* Component = static_cast<DTransactionLifecycleScene*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	Component->bUnsupportedInstanceData = true;
	FComponentInstanceDataCache Cache(*BeforeActor);
	FObjectReplacementMap Map; ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	EXPECT_FALSE(Cache.FindAndReplaceInstances(Map));
	EXPECT_TRUE(Cache.Validate(BeforeActor));
	EXPECT_FALSE(Cache.Validate(AfterActor));
}

TEST_F(FTransactionReinstancingTests, ComponentCacheRejectsAssignableButDifferentExactClass)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<AActor>(Current, "Actor");
	auto* AfterActor = NewObject<AActor>(Candidate, "Actor");
	const auto Key = DTransactionConstructionActor::Key(0);
	auto* Scene = static_cast<DSceneComponent*>(BeforeActor->AddComponentByClass(DSceneComponent::StaticClass(), false, FTransform(), false, Key, "Generated"));
	Scene->SetRelativeLocation({17.0, 0.0, 0.0});
	FComponentInstanceDataCache Cache(*BeforeActor);
	ASSERT_NE(AfterActor->AddComponentByClass(DTransactionLifecycleScene::StaticClass(), false, FTransform(), false, Key, "Generated"), nullptr);
	FObjectReplacementMap Map; ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	EXPECT_FALSE(Cache.FindAndReplaceInstances(Map));
	EXPECT_TRUE(Cache.Validate(BeforeActor));
}

TEST_F(FTransactionReinstancingTests, RetiredCommandDestructionWaitsForRetirementAndSeesPinnedOldObjects)
{
	using namespace Durin;
	ASSERT_EQ(Buffer->Begin("test", "Retired destructor"), 0);
	ASSERT_TRUE(GUndo->StoreUndo(Old, std::make_unique<FReinstancedValueCommand>(Old, 0, 12, false)));
	Old->Value = 12; ASSERT_EQ(Buffer->End(), 1);
	int32 Destructions = 0;
	bool bSawLiveOld = false;
	FReinstancedValueCommand::DestructionObserver = [&](DObject* Object) { ++Destructions; bSawLiveOld = Object == Old && IsValid(Object); };
	struct FResetObserver { ~FResetObserver() { FReinstancedValueCommand::DestructionObserver = {}; } } ResetObserver;
	ASSERT_TRUE(Prepare()); EXPECT_EQ(Destructions, 0);
	ASSERT_TRUE(Operation.TryCommit()); EXPECT_EQ(Destructions, 0);
	ASSERT_TRUE(Operation.Retire());
	EXPECT_EQ(Destructions, 1); EXPECT_TRUE(bSawLiveOld);
	FReinstancedValueCommand::DestructionObserver = {};
}

TEST_F(FTransactionReinstancingTests, SerializedGeneratedSourceRejectsAmbiguousOwnerIdentity)
{
	using namespace Durin;
	auto* Actor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* OtherOwner = NewObject<AActor>(nullptr, "AmbiguousSourceOwner");
	FByteBuffer Bytes;
	FObjectMemoryWriter Writer(Bytes, EArchivePurpose::Transaction);
	FObjectKey References[]{FObjectKey{}, FObjectKey(Actor), FObjectKey(Actor->GetRootComponent()->GetClass()), FObjectKey(OtherOwner)};
	for (auto& Reference : References) Reference.SerializeForSnapshot(Writer);
	auto Key = DTransactionConstructionActor::Key(0);
	Writer << Key.Namespace << Key.Id.A << Key.Id.B << Key.Id.C << Key.Id.D;
	FObjectMemoryReader Reader(Bytes, EArchivePurpose::Transaction);
	FActorComponentInstanceSourceInfo Source;
	Source.Serialize(Reader);
	EXPECT_TRUE(Reader.IsError());
	EXPECT_FALSE(Source.Validate());
	FObjectReplacementMap Map;
	EXPECT_FALSE(Source.FindAndReplaceInstances(Map));
}

TEST_F(FTransactionReinstancingTests, UnaffectedNativeCommandMovesWithoutRequiringAReplacementHook)
{
	using namespace Durin; using namespace Durin::Editor;
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(nullptr, "UnaffectedNativePeer"));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Mixed affected and unaffected", Old});
		ASSERT_TRUE(Old->Modify()); Old->Value = 12;
		Peer->Value = 9;
		ASSERT_TRUE(GUndo->StoreUndo(Peer.Get(), std::make_unique<FReinstancedValueCommand>(Peer.Get(), 0, 9, false)));
		ASSERT_TRUE(Scope.End());
	}
	New->Value = 77;
	ASSERT_TRUE(Prepare()); ASSERT_TRUE(Operation.TryCommit());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 0); EXPECT_EQ(Peer->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 77); EXPECT_EQ(Peer->Value, 9);
}

TEST_F(FTransactionReinstancingTests, WeakOnlySerializedDependencyRebindsWithoutAcquiringCollectorOwnership)
{
	using namespace Durin; using namespace Durin::Editor;
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(nullptr, "WeakOnlyReinstancingPeer"));
	TStrongObjectPtr<DTransactionRecordParticipant> KeepNew(New);
	Peer->Weak = Old;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Weak-only dependency", Peer.Get()});
		ASSERT_TRUE(Peer->Modify()); Peer->Value = 12; ASSERT_TRUE(Scope.End());
	}
	ASSERT_TRUE(Prepare()); ASSERT_TRUE(Operation.TryCommit());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	Peer->Weak = nullptr;
	ASSERT_TRUE(Operation.Retire()); CollectGarbage();
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Peer->Weak.Get(), New);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_FALSE(Peer->Weak.IsValid());
}
