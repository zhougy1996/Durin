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
		std::vector<Durin::TObjectPtr<Durin::DObject>> References;
	};

	auto ContainsObject(const Durin::DObject* Object) -> bool
	{
		return Durin::GDObjectArray.Contains(Object);
	}

	TEST(FPropertyValueSnapshotTests, RestoresScalarAndStringValues)
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
		FSnapshotOwner Owner{17, "before"};
		Durin::FPropertyValueSnapshot ValueSnapshot;
		Durin::FPropertyValueSnapshot LabelSnapshot;
		std::string Error;

		ASSERT_TRUE(Durin::CapturePropertyValue(&ValueProperty, &Owner, 0, ValueSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::CapturePropertyValue(&LabelProperty, &Owner, 0, LabelSnapshot, &Error)) << Error;
		Owner.Value = 91;
		Owner.Label = "after";
		ASSERT_TRUE(Durin::RestorePropertyValue(&ValueProperty, &Owner, 0, ValueSnapshot, &Error)) << Error;
		ASSERT_TRUE(Durin::RestorePropertyValue(&LabelProperty, &Owner, 0, LabelSnapshot, &Error)) << Error;

		EXPECT_EQ(Owner.Value, 17);
		EXPECT_EQ(Owner.Label, "before");
		Durin::FPropertyValueSnapshot Duplicate = LabelSnapshot;
		EXPECT_EQ(Duplicate, LabelSnapshot);
		EXPECT_FALSE(Durin::RestorePropertyValue(&ValueProperty, &Owner, 0, LabelSnapshot, &Error));
		EXPECT_FALSE(Error.empty());
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
