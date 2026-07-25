#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/ObjectPtr.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

#include <gtest/gtest.h>

namespace
{
	void EnsureSnapshotTestsInitialized()
	{
		static const bool bInitialized = [] {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			if (!Durin::IsFNameInitialized()) Durin::FNameInit();
			if (!Durin::FindClassByQualifiedName("Durin::DObject")) Durin::DObjectInit();
			return true;
		}();
		(void)bInitialized;
	}

	template<typename T>
	auto VectorNum(const void* Container) -> Durin::uint64
	{
		return static_cast<Durin::uint64>(static_cast<const std::vector<T>*>(Container)->size());
	}

	template<typename T>
	auto VectorElement(const void* Container, Durin::uint64 Index) -> const void*
	{
		return &(*static_cast<const std::vector<T>*>(Container))[static_cast<size_t>(Index)];
	}

	template<typename T>
	auto MutableVectorElement(void* Container, Durin::uint64 Index) -> void*
	{
		return &(*static_cast<std::vector<T>*>(Container))[static_cast<size_t>(Index)];
	}

	template<typename T>
	auto ResizeVector(void* Container, Durin::uint64 Num) -> void
	{
		static_cast<std::vector<T>*>(Container)->resize(static_cast<size_t>(Num));
	}

	template<typename T>
	const Durin::DurinCodeGen::FArrayPropertyHelper GSnapshotVectorHelper = {
		&VectorNum<T>,
		&VectorElement<T>,
		&MutableVectorElement<T>,
		&ResizeVector<T>
	};

	struct FSnapshotOwner
	{
		Durin::int32 Value = 0;
		std::string Label;
		Durin::FName Name;
		Durin::FGuid Guid;
		std::vector<Durin::FGuid> Guids;
		std::vector<Durin::TObjectPtr<Durin::DObject>> References;
	};

	struct FNestedGuid
	{
		Durin::FGuid Value;
	};

	struct FNestedGuidOwner
	{
		FNestedGuid Nested;
	};

	auto ContainsObject(const Durin::DObject* Object) -> bool
	{
		return Durin::GDObjectArray.Contains(Object);
	}

	TEST(FPropertyValueSnapshotTests, RestoresScalarStringAndNameValues)
	{
		EnsureSnapshotTestsInitialized();
		Durin::FNumericProperty ValueProperty(
			Durin::FFieldVariant(), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::int32)), Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
		);
		Durin::FStringProperty LabelProperty(
			Durin::FFieldVariant(), Durin::FName("Label"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Label)),
			static_cast<Durin::uint16>(sizeof(std::string)), Durin::DurinCodeGen::EPropertyGenFlags::String, nullptr
		);
		Durin::FNameProperty NameProperty(
			Durin::FFieldVariant(), Durin::FName("Name"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Name)),
			static_cast<Durin::uint16>(sizeof(Durin::FName)), Durin::DurinCodeGen::EPropertyGenFlags::Name, nullptr
		);
		FSnapshotOwner Owner;
		Owner.Value = 17;
		Owner.Label = "before";
		Owner.Name = Durin::FName("BeforeName_3");
		Durin::FPropertyValueSnapshot ValueSnapshot;
		Durin::FPropertyValueSnapshot LabelSnapshot;
		Durin::FPropertyValueSnapshot NameSnapshot;
		std::string Error;

		ASSERT_TRUE(Durin::CapturePropertyValue(&ValueProperty, &Owner, 0, ValueSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&LabelProperty, &Owner, 0, LabelSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&NameProperty, &Owner, 0, NameSnapshot, &Error)) << Error;
		Owner.Value = 91;
		Owner.Label = "after";
		Owner.Name = Durin::FName("AfterName");
		ASSERT_TRUE(Durin::RestorePropertyValue(&ValueProperty, &Owner, 0, ValueSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&LabelProperty, &Owner, 0, LabelSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&NameProperty, &Owner, 0, NameSnapshot, &Error)) << Error;

		EXPECT_EQ(Owner.Value, 17);
		EXPECT_EQ(Owner.Label, "before");
		EXPECT_EQ(Owner.Name.ToString(), "BeforeName_3");
		Durin::FPropertyValueSnapshot Duplicate = LabelSnapshot;
		EXPECT_EQ(Duplicate, LabelSnapshot);
		EXPECT_FALSE(Durin::RestorePropertyValue(&ValueProperty, &Owner, 0, LabelSnapshot, &Error));
		EXPECT_FALSE(Error.empty());
	}

	TEST(FPropertyValueSnapshotTests, RestoresDirectNestedAndArrayGuidValuesByteForByte)
	{
		EnsureSnapshotTestsInitialized();
		Durin::FGuidProperty GuidProperty(
			Durin::FFieldVariant(), Durin::FName("Guid"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Guid)),
			static_cast<Durin::uint16>(sizeof(Durin::FGuid)), Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		Durin::FArrayProperty GuidArrayProperty(
			Durin::FFieldVariant(), Durin::FName("Guids"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, Guids)),
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::FGuid>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr, &GSnapshotVectorHelper<Durin::FGuid>
		);
		Durin::FGuidProperty GuidArrayInner(
			Durin::FFieldVariant(&GuidArrayProperty), Durin::FName("Guids_Inner"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, static_cast<Durin::uint16>(sizeof(Durin::FGuid)),
			Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		GuidArrayProperty.SetInner(&GuidArrayInner);

		Durin::DStruct NestedStruct(
			Durin::EC_StaticConstructor, Durin::FName("Tests::FNestedGuid"), Durin::FName("FNestedGuid"),
			sizeof(FNestedGuid), alignof(FNestedGuid), Durin::EObjectFlags::Transient
		);
		Durin::FGuidProperty NestedValueProperty(
			Durin::FFieldVariant(&NestedStruct), Durin::FName("Value"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FNestedGuid, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::FGuid)), Durin::DurinCodeGen::EPropertyGenFlags::Guid, nullptr
		);
		NestedStruct.ChildProperties = &NestedValueProperty;
		Durin::FStructProperty NestedProperty(
			Durin::FFieldVariant(), Durin::FName("Nested"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FNestedGuidOwner, Nested)),
			static_cast<Durin::uint16>(sizeof(FNestedGuid)), Durin::DurinCodeGen::EPropertyGenFlags::Struct, &NestedStruct
		);

		const Durin::FGuid Direct(0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff);
		const std::vector<Durin::FGuid> Array{
			Durin::FGuid(1, 2, 3, 4), Durin::FGuid(0xffffffff, 0xabcdef01, 0x23456789, 0x87654321)
		};
		const Durin::FGuid Nested(0xdeadbeef, 0xcafebabe, 0x10203040, 0x50607080);
		FSnapshotOwner Owner;
		Owner.Guid = Direct;
		Owner.Guids = Array;
		FNestedGuidOwner NestedOwner{{Nested}};
		Durin::FPropertyValueSnapshot DirectSnapshot, ArraySnapshot, NestedSnapshot;
		std::string Error;

		ASSERT_TRUE(Durin::CapturePropertyValue(&GuidProperty, &Owner, 0, DirectSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&GuidArrayProperty, &Owner, 0, ArraySnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&NestedProperty, &NestedOwner, 0, NestedSnapshot, &Error)) << Error;
		EXPECT_EQ(DirectSnapshot.GetBytes().size(), sizeof(Durin::FGuid));
		Owner.Guid = {};
		Owner.Guids = {Durin::FGuid(9, 9, 9, 9)};
		NestedOwner.Nested.Value = {};
		ASSERT_TRUE(Durin::RestorePropertyValue(&GuidProperty, &Owner, 0, DirectSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&GuidArrayProperty, &Owner, 0, ArraySnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&NestedProperty, &NestedOwner, 0, NestedSnapshot, &Error)) << Error;

		EXPECT_EQ(Owner.Guid, Direct);
		EXPECT_EQ(Owner.Guids, Array);
		EXPECT_EQ(NestedOwner.Nested.Value, Nested);
	}

	TEST(FPropertyValueSnapshotTests, KeepsNestedObjectReferencesAliveUntilReleased)
	{
		EnsureSnapshotTestsInitialized();
		Durin::FArrayProperty ReferencesProperty(
			Durin::FFieldVariant(), Durin::FName("References"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, static_cast<Durin::uint16>(offsetof(FSnapshotOwner, References)),
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::TObjectPtr<Durin::DObject>>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array, nullptr,
			&GSnapshotVectorHelper<Durin::TObjectPtr<Durin::DObject>>
		);
		Durin::FObjectProperty InnerProperty(
			Durin::FFieldVariant(&ReferencesProperty), Durin::FName("References_Inner"), Durin::EObjectFlags::NoFlags,
			Durin::EPropertyFlags::None, 1, 0, static_cast<Durin::uint16>(sizeof(Durin::TObjectPtr<Durin::DObject>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object, Durin::DObject::StaticClass(), true
		);
		ReferencesProperty.SetInner(&InnerProperty);

		Durin::DObject* First = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SnapshotReferenceA"));
		Durin::DObject* Second = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("SnapshotReferenceB"));
		FSnapshotOwner Owner;
		Owner.References = {First, Second, First};
		Durin::FPropertyValueSnapshot Snapshot;
		std::string Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&ReferencesProperty, &Owner, 0, Snapshot, &Error)) << Error;
		ASSERT_EQ(Snapshot.GetReferencedObjects().size(), 2u);
		Durin::FPropertyValueSnapshot SnapshotCopy = Snapshot;
		Snapshot = {};

		Owner.References.clear();
		Durin::CollectGarbage();
		ASSERT_TRUE(ContainsObject(First));
		ASSERT_TRUE(ContainsObject(Second));
		ASSERT_TRUE(Durin::RestorePropertyValue(&ReferencesProperty, &Owner, 0, SnapshotCopy, &Error)) << Error;
		ASSERT_EQ(Owner.References.size(), 3u);
		EXPECT_EQ(Owner.References[0].Get(), First);
		EXPECT_EQ(Owner.References[1].Get(), Second);
		EXPECT_EQ(Owner.References[2].Get(), First);

		Owner.References.clear();
		SnapshotCopy = {};
		Durin::CollectGarbage();
		EXPECT_FALSE(ContainsObject(First));
		EXPECT_FALSE(ContainsObject(Second));
	}
}
