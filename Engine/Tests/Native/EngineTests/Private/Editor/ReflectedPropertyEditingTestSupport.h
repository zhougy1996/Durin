#pragma once

#include "Editor/PropertyEditing.h"
#include "Editor/Transaction.h"
#include "Editor/Transactor.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/AssetPath.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Components/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "EngineTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	class FTestTransactorOwner
	{
	public:
		FTestTransactorOwner()
			: Transactor(Durin::NewObject<Durin::DTransBuffer>(nullptr, MakeName()))
			, TransactorRoot(Transactor)
		{}

		auto Get() const -> Durin::DTransBuffer* { return Transactor; }

	private:
		static auto MakeName() -> Durin::FName
		{
			static uint64 NextId = 1;
			return Durin::FName("ReflectedPropertyTransactor" + std::to_string(NextId++));
		}

		Durin::DTransBuffer* Transactor = nullptr;
		Durin::TStrongObjectPtr<Durin::DObject> TransactorRoot;
	};

	struct FValueContainer
	{
		int32 Value = 0;
	};

	struct FGuidValueContainer
	{
		Durin::FGuid Value;
	};

	struct FObjectValueContainer
	{
		Durin::TObjectPtr<Durin::DObject> Value;
	};

	struct FArrayValueContainer
	{
		std::vector<int32> Values;
	};

	struct FMapValueContainer
	{
		std::unordered_map<std::string, int32> Values;
	};

	template<typename T>
	auto InitializeTestValue(void* Memory) -> void { std::construct_at(static_cast<T*>(Memory)); }
	template<typename T>
	auto DestroyTestValue(void* Memory) -> void { std::destroy_at(static_cast<T*>(Memory)); }
	template<typename T>
	auto SetTestValueLifecycle(Durin::FProperty& Property) -> void
	{
		Property.SetValueLifecycle(sizeof(T), alignof(T), &InitializeTestValue<T>, &DestroyTestValue<T>);
	}

	struct FCapturedChange
	{
		Durin::EPropertyChangePhase Phase = Durin::EPropertyChangePhase::Committed;
		Durin::EPropertyChangeKind Kind = Durin::EPropertyChangeKind::ValueSet;
		Durin::EPropertyChangeOrigin Origin = Durin::EPropertyChangeOrigin::Edit;
		const Durin::FProperty* MemberProperty = nullptr;
		const Durin::FProperty* LeafProperty = nullptr;
		std::vector<Durin::EPropertyPathSelector> Selectors;
		std::vector<uint64> Indices;
		std::vector<std::byte> MapKeyData;
	};

	class DEditObserver : public Durin::DObject
	{
	public:
		explicit DEditObserver(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DObject(Initializer)
		{}

		auto PreEditChangeProperty(Durin::FPropertyEditProposal& Proposal, std::string& OutError) -> bool override
		{
			++PreChangeCount;
			LastProposalPhase = Proposal.Phase;
			LastProposalOrigin = Proposal.Origin;
			LastProposalKind = Proposal.Kind;
			bLastProposalHadLeaf = Proposal.DraftLeafContainer != nullptr;
			return PreChange ? PreChange(Proposal, OutError) : true;
		}

		auto PostEditChangeProperty(const Durin::FPropertyChangedEvent& Event) -> void override
		{
			FCapturedChange& Change = Changes.emplace_back();
			Change.Phase = Event.Phase;
			Change.Kind = Event.Kind;
			Change.Origin = Event.Origin;
			Change.MemberProperty = Event.MemberProperty;
			Change.LeafProperty = Event.LeafProperty;
			for (const Durin::FPropertyPathSegment& Segment : Event.Path)
			{
				Change.Selectors.push_back(Segment.Selector);
				Change.Indices.push_back(Segment.Index);
				if (!Segment.MapKeyData.empty()) Change.MapKeyData.assign(Segment.MapKeyData.begin(), Segment.MapKeyData.end());
			}
		}

		std::vector<FCapturedChange> Changes;
		std::function<bool(Durin::FPropertyEditProposal&, std::string&)> PreChange;
		uint32 PreChangeCount = 0;
		Durin::EPropertyChangePhase LastProposalPhase = Durin::EPropertyChangePhase::Interactive;
		Durin::EPropertyChangeOrigin LastProposalOrigin = Durin::EPropertyChangeOrigin::Edit;
		Durin::EPropertyChangeKind LastProposalKind = Durin::EPropertyChangeKind::ValueSet;
		bool bLastProposalHadLeaf = false;
	};

	class DReflectedTransactionTestObject final : public DEditObserver
	{
	public:
		explicit DReflectedTransactionTestObject(
			const Durin::FObjectInitializer& Initializer = Durin::FObjectInitializer::Get())
			: DEditObserver(Initializer)
		{}

		static void __DefaultConstructor(const Durin::FObjectInitializer& Initializer)
		{
			new (Initializer.GetObj()) DReflectedTransactionTestObject(Initializer);
		}

		static auto StaticClass() -> Durin::DClass*
		{
			static Durin::DClass* Class = nullptr;
			if (Class) return Class;
			Class = new Durin::DClass(
				Durin::EC_StaticConstructor, Durin::FName("Tests::DReflectedTransactionTestObject"),
				sizeof(DReflectedTransactionTestObject), alignof(DReflectedTransactionTestObject),
				Durin::EObjectFlags::NoFlags, Durin::EClassFlags::None,
				Durin::EClassCastFlags::DClass,
				(Durin::DClass::ClassConstructorType)
					Durin::InternalConstructor<DReflectedTransactionTestObject>);
			Class->SetSuperStructBase(Durin::DObject::StaticClass());
			Class->SetTypeNames("DReflectedTransactionTestObject", "", "");

			auto* ValueProperty = new Durin::FNumericProperty(
				Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1,
				STRUCT_OFFSET_UINT16(DReflectedTransactionTestObject, Value), sizeof(int32),
				Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr);
			SetTestValueLifecycle<int32>(*ValueProperty);
			auto* GuidProperty = new Durin::FGuidProperty(
				Durin::FFieldVariant(Class), Durin::FName("GuidValue"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1,
				STRUCT_OFFSET_UINT16(DReflectedTransactionTestObject, GuidValue), sizeof(Durin::FGuid),
				Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr);
			SetTestValueLifecycle<Durin::FGuid>(*GuidProperty);
			auto* ObjectProperty = new Durin::FObjectProperty(
				Durin::FFieldVariant(Class), Durin::FName("ObjectValue"), Durin::EObjectFlags::NoFlags,
				Durin::EPropertyFlags::Edit, 1,
				STRUCT_OFFSET_UINT16(DReflectedTransactionTestObject, ObjectValue),
				sizeof(Durin::TObjectPtr<Durin::DObject>),
				Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(), true,
				[](const void* ValueAddress) -> Durin::DObject* {
					return static_cast<const Durin::TObjectPtr<Durin::DObject>*>(ValueAddress)->Get();
				},
				[](void* ValueAddress, Durin::DObject* Object) {
					*static_cast<Durin::TObjectPtr<Durin::DObject>*>(ValueAddress) = Object;
				});
			SetTestValueLifecycle<Durin::TObjectPtr<Durin::DObject>>(*ObjectProperty);
			ValueProperty->Next = GuidProperty;
			GuidProperty->Next = ObjectProperty;
			Class->ChildProperties = ValueProperty;
			Class->Register(Durin::DClass::StaticClass, "", "DReflectedTransactionTestObject");
			Durin::DObjectForceRegistration(Class);
			return Class;
		}

		static auto FindProperty(std::string_view Name) -> Durin::FProperty*
		{
			return StaticClass()->FindPropertyByName(Durin::FName(Name));
		}

		int32 Value = 0;
		Durin::FGuid GuidValue;
		Durin::TObjectPtr<Durin::DObject> ObjectValue;
	};

	class FManagedEditObserver
	{
	private:
		static auto CreateObject() -> DReflectedTransactionTestObject*
		{
			InitializeDObjectSystem();
			static uint64 NextId = 1;
			return Durin::NewObject<DReflectedTransactionTestObject>(
				nullptr, Durin::FName("ManagedEditObserver" + std::to_string(NextId++)));
		}

		DReflectedTransactionTestObject* ManagedObject = nullptr;
		Durin::TStrongObjectPtr<Durin::DObject> StrongObject;

	public:
		FManagedEditObserver()
			: ManagedObject(CreateObject())
			, StrongObject(ManagedObject)
			, Changes(ManagedObject->Changes)
			, PreChange(ManagedObject->PreChange)
		{
		}

		operator DEditObserver&() const { return *ManagedObject; }
		auto Get() const -> DEditObserver* { return ManagedObject; }

		std::vector<FCapturedChange>& Changes;
		std::function<bool(Durin::FPropertyEditProposal&, std::string&)>& PreChange;
	};

	auto MakeValueProperty() -> std::unique_ptr<Durin::FNumericProperty>
	{
		auto Property = std::make_unique<Durin::FNumericProperty>(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, static_cast<uint16>(offsetof(FValueContainer, Value)),
			static_cast<uint16>(sizeof(int32)), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		SetTestValueLifecycle<int32>(*Property);
		return Property;
	}

	auto MakeGuidProperty() -> std::unique_ptr<Durin::FGuidProperty>
	{
		auto Property = std::make_unique<Durin::FGuidProperty>(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, static_cast<uint16>(offsetof(FGuidValueContainer, Value)),
			static_cast<uint16>(sizeof(Durin::FGuid)), Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		SetTestValueLifecycle<Durin::FGuid>(*Property);
		return Property;
	}

	template<typename T>
	auto VectorNum(const void* Container) -> uint64 { return static_cast<uint64>(static_cast<const std::vector<T>*>(Container)->size()); }
	template<typename T>
	auto VectorElement(const void* Container, uint64 Index) -> const void* { return &(*static_cast<const std::vector<T>*>(Container))[static_cast<size_t>(Index)]; }
	template<typename T>
	auto MutableVectorElement(void* Container, uint64 Index) -> void* { return &(*static_cast<std::vector<T>*>(Container))[static_cast<size_t>(Index)]; }
	template<typename T>
	auto ResizeVector(void* Container, uint64 Num) -> bool { static_cast<std::vector<T>*>(Container)->resize(static_cast<size_t>(Num)); return true; }


	using FStringIntMap = std::unordered_map<std::string, int32>;
	auto MapNum(const void* Container) -> uint64 { return static_cast<uint64>(static_cast<const FStringIntMap*>(Container)->size()); }
	auto MapKey(const void* Container, uint64 Index) -> const void* { auto It = static_cast<const FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->first; }
	auto MapValue(const void* Container, uint64 Index) -> const void* { auto It = static_cast<const FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->second; }
	auto MutableMapValue(void* Container, uint64 Index) -> void* { auto It = static_cast<FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->second; }
	auto ClearMap(void* Container) -> void { static_cast<FStringIntMap*>(Container)->clear(); }
	auto CreateMapKey() -> void* { return new std::string(); }
	auto CopyMapKey(const void* Key) -> void* { return new std::string(*static_cast<const std::string*>(Key)); }
	auto DestroyMapKey(void* Key) -> void { delete static_cast<std::string*>(Key); }
	auto CreateMapValue() -> void* { return new int32(); }
	auto DestroyMapValue(void* Value) -> void { delete static_cast<int32*>(Value); }
	auto InsertMap(void* Container, const void* Key, const void* Value) -> bool { static_cast<FStringIntMap*>(Container)->insert_or_assign(*static_cast<const std::string*>(Key), *static_cast<const int32*>(Value)); return true; }
	auto ContainsMap(const void* Container, const void* Key) -> bool { return static_cast<const FStringIntMap*>(Container)->contains(*static_cast<const std::string*>(Key)); }
	auto RenameMapKey(void* Container, const void* OldKey, const void* NewKey) -> bool
	{
		auto* Map = static_cast<FStringIntMap*>(Container);
		const std::string Old = *static_cast<const std::string*>(OldKey);
		const std::string New = *static_cast<const std::string*>(NewKey);
		if (Old == New || Map->contains(New)) return false;
		auto Node = Map->extract(Old);
		if (Node.empty()) return false;
		Node.key() = New;
		Map->insert(std::move(Node));
		return true;
	}
	auto RemoveMap(void* Container, const void* Key) -> bool { return static_cast<FStringIntMap*>(Container)->erase(*static_cast<const std::string*>(Key)) != 0; }


	auto MakeArrayProperty(Durin::FNumericProperty& Inner) -> std::unique_ptr<Durin::FArrayProperty>
	{
		auto Property = std::make_unique<Durin::FArrayProperty>(
			Durin::FFieldVariant(), Durin::FName("Values"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
			1, static_cast<uint16>(offsetof(FArrayValueContainer, Values)), static_cast<uint16>(sizeof(std::vector<int32>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, Durin::ResolveArrayOps<std::vector<int32>>()
		);
		Property->SetInner(&Inner);
		SetTestValueLifecycle<std::vector<int32>>(*Property);
		return Property;
	}

	auto MakeMapProperty(Durin::FStringProperty& Key, Durin::FNumericProperty& Value) -> std::unique_ptr<Durin::FMapProperty>
	{
		auto Property = std::make_unique<Durin::FMapProperty>(
			Durin::FFieldVariant(), Durin::FName("Values"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
			1, static_cast<uint16>(offsetof(FMapValueContainer, Values)), static_cast<uint16>(sizeof(FStringIntMap)),
			Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, Durin::ResolveMapOps<FStringIntMap>()
		);
		Property->SetKeyProp(&Key);
		Property->SetValueProp(&Value);
		SetTestValueLifecycle<FStringIntMap>(*Property);
		return Property;
	}

	auto CaptureValue(const Durin::FProperty* Property, FValueContainer& Container, int32 Value) -> Durin::FPropertyValueSnapshot
	{
		FValueContainer Proposed{Value};
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Property, &Proposed, 0, Snapshot));
		return Snapshot;
	}

	auto MakeTarget(DEditObserver& Object, const Durin::FProperty* Property, FValueContainer& Container) -> Durin::Editor::FPropertyEditTarget
	{
		Durin::Editor::FPropertyEditTarget Target;
		Target.Object = &Object;
		Target.MemberProperty = Property;
		Target.LeafProperty = Property;
		Target.SnapshotProperty = Property;
		Target.SnapshotContainer = &Container;
		Target.Path.push_back({Property});
		return Target;
	}

	auto MakeReflectedRevisionTestPackage() -> Durin::DPackage*
	{
		InitializeDObjectSystem();
		Durin::Testing::FScopedMountRegistryFixture MountFixture;
		Durin::Testing::RegisterMountPointForTests(
			"/ReflectedRevisionTests/",
			(Durin::Testing::GetTestWorkDirectory() / "ReflectedRevisionTests").generic_string() + "/"
		);
		static uint64 NextPackageId = 1;
		const std::string Name = "Package" + std::to_string(NextPackageId++);
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate("/ReflectedRevisionTests/" + Name, Path));
		Durin::DPackage* Package = Durin::NewObject<Durin::DPackage>(nullptr, Durin::FName(Name));
		Package->InitializeAssetPackage(Path);
		return Package;
	}
}
