#pragma once

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
#include "World/ObjectStateTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace Durin::Tests::ObjectState
{
	template<typename T>
	inline auto SetValueLifecycle(Durin::FProperty& Property) -> void
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
	inline auto BeginRecording(Durin::DTransactor& Buffer, Durin::Editor::FTransactionContext Context = {}) -> FRecordingStart
	{
		const auto Index = Buffer.Begin(Context.Name, Context.Description);
		Buffer.SetPrimaryUndoObject(Context.PrimaryObject);
		return {Index, Buffer.GetLastResult().TransactionId};
	}
	inline auto EndRecording(Durin::Editor::FScopedTransaction& Scope, Durin::DTransactor& Buffer) -> Durin::Editor::FTransactorResult
	{ (void)Scope.End(); return Buffer.GetLastResult(); }
	inline auto CancelRecording(Durin::Editor::FScopedTransaction& Scope, Durin::DTransactor& Buffer) -> Durin::Editor::FTransactorResult
	{ Scope.Cancel(); return Buffer.GetLastResult(); }
	inline auto CancelRecording(Durin::DTransactor& Buffer, int32 Index) -> Durin::Editor::FTransactorResult
	{ Buffer.Cancel(Index); return Buffer.GetLastResult(); }
	inline auto EndRecording(Durin::DTransactor& Buffer) -> Durin::Editor::FTransactorResult
	{ (void)Buffer.End(); return Buffer.GetLastResult(); }
	inline auto ReadTransactionEvents(Durin::DTransactor& Buffer) -> std::vector<Durin::Editor::FTransactionEvent>
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

	inline auto Property(std::string_view Name) -> Durin::FProperty*
	{
		return DTransactionRecordParticipant::StaticClass()->FindPropertyByName(Durin::FName(Name));
	}

	inline auto Contains(const Durin::DObject* Object) -> bool
	{
		return Durin::GDObjectArray.Contains(Object);
	}

	inline auto MakeSoftPath() -> Durin::FObjectPath
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

	inline auto CaptureRecord(DTransactionRecordParticipant* Target) -> Durin::Editor::FTransactionObjectRecord
	{
		auto Result = Durin::Editor::FTransactionObjectRecord::CaptureWhole(*Target);
		EXPECT_TRUE(Result) << (Result ? "" : Durin::Editor::FormatTransactionObjectRecordError(Result.error()));
		return Result ? std::move(*Result) : Durin::Editor::FTransactionObjectRecord{};
	}

}
