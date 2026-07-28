#pragma once

#include "Editor/ReflectedPropertyEditing.h"
#include "Editor/EditorTransaction.h"

#include "DObject/DurinPropertyTypes.h"
#include "DObject/DObjectArray.h"
#include "DObject/AssetPath.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "DObject/Package.h"
#include "Components/CameraComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstance.h"
#include "Misc/Paths.h"
#include "EngineTestSupport.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	struct FValueContainer
	{
		Durin::int32 Value = 0;
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
		std::vector<Durin::int32> Values;
	};

	struct FMapValueContainer
	{
		std::unordered_map<std::string, Durin::int32> Values;
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
		std::vector<Durin::uint64> Indices;
		std::vector<Durin::uint8> MapKeyData;
	};

	class DEditObserver final : public Durin::DObject
	{
	public:
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
		Durin::uint32 PreChangeCount = 0;
		Durin::EPropertyChangePhase LastProposalPhase = Durin::EPropertyChangePhase::Interactive;
		Durin::EPropertyChangeOrigin LastProposalOrigin = Durin::EPropertyChangeOrigin::Edit;
		Durin::EPropertyChangeKind LastProposalKind = Durin::EPropertyChangeKind::ValueSet;
		bool bLastProposalHadLeaf = false;
	};

	auto MakeValueProperty() -> std::unique_ptr<Durin::FNumericProperty>
	{
		auto Property = std::make_unique<Durin::FNumericProperty>(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, static_cast<Durin::uint16>(offsetof(FValueContainer, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::int32)), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		SetTestValueLifecycle<Durin::int32>(*Property);
		return Property;
	}

	auto MakeGuidProperty() -> std::unique_ptr<Durin::FGuidProperty>
	{
		auto Property = std::make_unique<Durin::FGuidProperty>(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::Edit, 1, static_cast<Durin::uint16>(offsetof(FGuidValueContainer, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::FGuid)), Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		SetTestValueLifecycle<Durin::FGuid>(*Property);
		return Property;
	}

	template<typename T>
	auto VectorNum(const void* Container) -> Durin::uint64 { return static_cast<Durin::uint64>(static_cast<const std::vector<T>*>(Container)->size()); }
	template<typename T>
	auto VectorElement(const void* Container, Durin::uint64 Index) -> const void* { return &(*static_cast<const std::vector<T>*>(Container))[static_cast<size_t>(Index)]; }
	template<typename T>
	auto MutableVectorElement(void* Container, Durin::uint64 Index) -> void* { return &(*static_cast<std::vector<T>*>(Container))[static_cast<size_t>(Index)]; }
	template<typename T>
	auto ResizeVector(void* Container, Durin::uint64 Num) -> void { static_cast<std::vector<T>*>(Container)->resize(static_cast<size_t>(Num)); }

	const Durin::DurinCodeGen::FArrayPropertyHelper GIntArrayHelper = {
		&VectorNum<Durin::int32>, &VectorElement<Durin::int32>, &MutableVectorElement<Durin::int32>, &ResizeVector<Durin::int32>
	};

	using FStringIntMap = std::unordered_map<std::string, Durin::int32>;
	auto MapNum(const void* Container) -> Durin::uint64 { return static_cast<Durin::uint64>(static_cast<const FStringIntMap*>(Container)->size()); }
	auto MapKey(const void* Container, Durin::uint64 Index) -> const void* { auto It = static_cast<const FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->first; }
	auto MapValue(const void* Container, Durin::uint64 Index) -> const void* { auto It = static_cast<const FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->second; }
	auto MutableMapValue(void* Container, Durin::uint64 Index) -> void* { auto It = static_cast<FStringIntMap*>(Container)->begin(); std::advance(It, static_cast<size_t>(Index)); return &It->second; }
	auto ClearMap(void* Container) -> void { static_cast<FStringIntMap*>(Container)->clear(); }
	auto CreateMapKey() -> void* { return new std::string(); }
	auto CopyMapKey(const void* Key) -> void* { return new std::string(*static_cast<const std::string*>(Key)); }
	auto DestroyMapKey(void* Key) -> void { delete static_cast<std::string*>(Key); }
	auto CreateMapValue() -> void* { return new Durin::int32(); }
	auto DestroyMapValue(void* Value) -> void { delete static_cast<Durin::int32*>(Value); }
	auto InsertMap(void* Container, const void* Key, const void* Value) -> void { static_cast<FStringIntMap*>(Container)->insert_or_assign(*static_cast<const std::string*>(Key), *static_cast<const Durin::int32*>(Value)); }
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

	const Durin::DurinCodeGen::FMapPropertyHelper GStringIntMapHelper = {
		&MapNum, &MapKey, &MapValue, &MutableMapValue, &ClearMap,
		&CreateMapKey, &CopyMapKey, &DestroyMapKey, &CreateMapValue, &DestroyMapValue,
		&InsertMap, &ContainsMap, &RenameMapKey, &RemoveMap
	};

	auto MakeArrayProperty(Durin::FNumericProperty& Inner) -> std::unique_ptr<Durin::FArrayProperty>
	{
		auto Property = std::make_unique<Durin::FArrayProperty>(
			Durin::FFieldVariant(), Durin::FName("Values"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
			1, static_cast<Durin::uint16>(offsetof(FArrayValueContainer, Values)), static_cast<Durin::uint16>(sizeof(std::vector<Durin::int32>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, &GIntArrayHelper
		);
		Property->SetInner(&Inner);
		SetTestValueLifecycle<std::vector<Durin::int32>>(*Property);
		return Property;
	}

	auto MakeMapProperty(Durin::FStringProperty& Key, Durin::FNumericProperty& Value) -> std::unique_ptr<Durin::FMapProperty>
	{
		auto Property = std::make_unique<Durin::FMapProperty>(
			Durin::FFieldVariant(), Durin::FName("Values"), Durin::EObjectFlags::NoFlags, Durin::EPropertyFlags::Edit,
			1, static_cast<Durin::uint16>(offsetof(FMapValueContainer, Values)), static_cast<Durin::uint16>(sizeof(FStringIntMap)),
			Durin::DurinCodeGen::EPropertyGenFlags::Map, nullptr, &GStringIntMapHelper
		);
		Property->SetKeyProp(&Key);
		Property->SetValueProp(&Value);
		SetTestValueLifecycle<FStringIntMap>(*Property);
		return Property;
	}

	auto CaptureValue(const Durin::FProperty* Property, FValueContainer& Container, Durin::int32 Value) -> Durin::FPropertyValueSnapshot
	{
		FValueContainer Proposed{Value};
		Durin::FPropertyValueSnapshot Snapshot;
		EXPECT_TRUE(Durin::CapturePropertyValue(Property, &Proposed, 0, Snapshot));
		return Snapshot;
	}

	auto MakeTarget(DEditObserver& Object, const Durin::FProperty* Property, FValueContainer& Container) -> Durin::FReflectedPropertyEditTarget
	{
		Durin::FReflectedPropertyEditTarget Target;
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
		Durin::PathUtilities::FScopedMountRegistryFixture MountFixture;
		Durin::PathUtilities::RegisterMountPoint(
			"/ReflectedRevisionTests/",
			Durin::Testing::GetTestWorkDirectory().generic_string() + "/"
		);
		static Durin::uint64 NextPackageId = 1;
		const std::string Name = "Package" + std::to_string(NextPackageId++);
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate("/ReflectedRevisionTests/" + Name, Path));
		Durin::DPackage* Package = Durin::NewObject<Durin::DPackage>(nullptr, Durin::FName(Name));
		Package->InitializeAssetPackage(Path);
		return Package;
	}
}
