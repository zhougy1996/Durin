#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Object.h"
#include "DObject/DurinPropertyTypes.h"

#include <gtest/gtest.h>
#include <cstddef>

namespace
{
	struct FReflectedPropertyOwnerForTest
	{
		Durin::int32 Value = 0;
		Durin::DObject* ObjectValue = nullptr;
		std::string StringValue;
		std::vector<Durin::DObject*> ObjectArray;
		std::unordered_map<std::string, Durin::int32> StringToInt;
		std::vector<std::vector<Durin::int32>> NestedScores;
		std::unordered_map<std::string, std::vector<Durin::DObject*>> ObjectLists;
	};

	Durin::DClass* Z_Construct_DClass_FReflectedPropertyOwnerForTest_NoRegister()
	{
		static Durin::DClass* Class = new Durin::DClass(
			Durin::EC_StaticConstructor,
			Durin::FName("FReflectedPropertyOwnerForTest"),
			sizeof(FReflectedPropertyOwnerForTest),
			alignof(FReflectedPropertyOwnerForTest),
			Durin::EObjectFlags::NoFlags,
			Durin::EClassFlags::None,
			Durin::EClassCastFlags::DClass,
			nullptr
		);
		return Class;
	}

	void EnsureDObjectInitialized()
	{
		static const bool bInitialized = []()
		{
			Durin::DObjectInit();
			return true;
		}();
		(void)bInitialized;
	}

	TEST(FCoreDObjectReflectionTests, IntrinsicTypesUseTypeAndStructBaseHierarchy)
	{
		EnsureDObjectInitialized();

		EXPECT_EQ(Durin::DType::StaticClass()->GetSuperClass(), Durin::DObject::StaticClass());
		EXPECT_EQ(Durin::DStructBase::StaticClass()->GetSuperClass(), Durin::DType::StaticClass());
		EXPECT_EQ(Durin::DClass::StaticClass()->GetSuperClass(), Durin::DStructBase::StaticClass());
	}

	TEST(FCoreDObjectReflectionTests, NewObjectKeepsClassAndCastBehavior)
	{
		EnsureDObjectInitialized();

		Durin::DObject* Object = Durin::NewObject<Durin::DObject>(nullptr, Durin::FName("CoreDObjectTestObject"));

		ASSERT_NE(Object, nullptr);
		EXPECT_EQ(Object->GetClass(), Durin::DObject::StaticClass());
		EXPECT_TRUE(Object->IsA(Durin::DObject::StaticClass()));
		EXPECT_EQ(Durin::Cast<Durin::DObject>(Object), Object);
	}

	TEST(FCoreDObjectReflectionTests, ConstructDClassAttachesGeneratedPropertiesToStructBase)
	{
		static const Durin::DurinCodeGen::FInt32PropertyParams ValuePropertyParams = {
			"Value",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, Value)),
			static_cast<Durin::uint16>(sizeof(Durin::int32)),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectPropertyParams = {
			"ObjectValue",
			Durin::EPropertyFlags::Edit,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectValue)),
			static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object,
			&Durin::DObject::StaticClass,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FStringPropertyParams StringPropertyParams = {
			"StringValue",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, StringValue)),
			static_cast<Durin::uint16>(sizeof(std::string)),
			Durin::DurinCodeGen::EPropertyGenFlags::String,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectArrayInnerPropertyParams = {
			"ObjectArray_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object,
			&Durin::DObject::StaticClass,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams ObjectArrayPropertyParams = {
			"ObjectArray",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectArray)),
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::DObject*>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			&ObjectArrayInnerPropertyParams,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FStringPropertyParams StringToIntKeyPropertyParams = {
			"StringToInt_Key",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(std::string)),
			Durin::DurinCodeGen::EPropertyGenFlags::String,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FInt32PropertyParams StringToIntValuePropertyParams = {
			"StringToInt_Value",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::int32)),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FMapPropertyParams StringToIntPropertyParams = {
			"StringToInt",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, StringToInt)),
			static_cast<Durin::uint16>(sizeof(std::unordered_map<std::string, Durin::int32>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Map,
			nullptr,
			nullptr,
			&StringToIntKeyPropertyParams,
			&StringToIntValuePropertyParams
		};
		static const Durin::DurinCodeGen::FInt32PropertyParams NestedScoresInnerInnerPropertyParams = {
			"NestedScores_Inner_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::int32)),
			Durin::DurinCodeGen::EPropertyGenFlags::Int32,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams NestedScoresInnerPropertyParams = {
			"NestedScores_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::int32>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			&NestedScoresInnerInnerPropertyParams,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams NestedScoresPropertyParams = {
			"NestedScores",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, NestedScores)),
			static_cast<Durin::uint16>(sizeof(std::vector<std::vector<Durin::int32>>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			&NestedScoresInnerPropertyParams,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FStringPropertyParams ObjectListsKeyPropertyParams = {
			"ObjectLists_Key",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(std::string)),
			Durin::DurinCodeGen::EPropertyGenFlags::String,
			nullptr,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FObjectPropertyParams ObjectListsValueInnerPropertyParams = {
			"ObjectLists_Value_Inner",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(Durin::DObject*)),
			Durin::DurinCodeGen::EPropertyGenFlags::Object,
			&Durin::DObject::StaticClass,
			nullptr,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FArrayPropertyParams ObjectListsValuePropertyParams = {
			"ObjectLists_Value",
			Durin::EPropertyFlags::None,
			1,
			0,
			static_cast<Durin::uint16>(sizeof(std::vector<Durin::DObject*>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Array,
			nullptr,
			&ObjectListsValueInnerPropertyParams,
			nullptr,
			nullptr
		};
		static const Durin::DurinCodeGen::FMapPropertyParams ObjectListsPropertyParams = {
			"ObjectLists",
			Durin::EPropertyFlags::None,
			1,
			static_cast<Durin::uint16>(offsetof(FReflectedPropertyOwnerForTest, ObjectLists)),
			static_cast<Durin::uint16>(sizeof(std::unordered_map<std::string, std::vector<Durin::DObject*>>)),
			Durin::DurinCodeGen::EPropertyGenFlags::Map,
			nullptr,
			nullptr,
			&ObjectListsKeyPropertyParams,
			&ObjectListsValuePropertyParams
		};
		static const Durin::DurinCodeGen::FPropertyParamsBase* const PropertyParams[] = {
			&ValuePropertyParams,
			&ObjectPropertyParams,
			&StringPropertyParams,
			&ObjectArrayPropertyParams,
			&StringToIntPropertyParams,
			&NestedScoresPropertyParams,
			&ObjectListsPropertyParams
		};
		static const Durin::DurinCodeGen::FClassParams ClassParams = {
			&Z_Construct_DClass_FReflectedPropertyOwnerForTest_NoRegister,
			"FReflectedPropertyOwnerForTest",
			"FReflectedPropertyOwnerForTest",
			PropertyParams,
			7
		};

		Durin::DClass* Class = Durin::DurinCodeGen::ConstructDClass(ClassParams);

		ASSERT_NE(Class, nullptr);
		ASSERT_NE(Class->ChildProperties, nullptr);
		EXPECT_EQ(Class->ChildProperties->NamePrivate.ToString(), "Value");
		EXPECT_NE(Class->ChildProperties->Next, nullptr);
		EXPECT_EQ(Class->PropertiesSize, sizeof(FReflectedPropertyOwnerForTest));
		EXPECT_EQ(Class->MinAlignment, alignof(FReflectedPropertyOwnerForTest));

		auto* ValueProperty = Class->FindPropertyByName("Value");
		ASSERT_NE(ValueProperty, nullptr);
		EXPECT_EQ(ValueProperty->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Int32);
		EXPECT_EQ(ValueProperty->GetElementSize(), sizeof(Durin::int32));
		EXPECT_TRUE(ValueProperty->ClassPrivate->IsChildOf(Durin::FNumericProperty::StaticClass()));

		FReflectedPropertyOwnerForTest Instance;
		*ValueProperty->ContainerPtrToValuePtr<Durin::int32>(&Instance) = 42;
		EXPECT_EQ(Instance.Value, 42);

		auto* ObjectProperty = static_cast<Durin::FObjectProperty*>(Class->FindPropertyByName("ObjectValue"));
		ASSERT_NE(ObjectProperty, nullptr);
		EXPECT_EQ(ObjectProperty->GetReferencedClass(), Durin::DObject::StaticClass());
		EXPECT_TRUE(ObjectProperty->HasAnyPropertyFlags(Durin::EPropertyFlags::Edit));

		Durin::DObject ReferencedObject;
		Instance.ObjectValue = &ReferencedObject;
		EXPECT_EQ(ObjectProperty->GetObjectPropertyValue(&Instance), &ReferencedObject);

		auto* StringProperty = static_cast<Durin::FStringProperty*>(Class->FindPropertyByName("StringValue"));
		ASSERT_NE(StringProperty, nullptr);
		*StringProperty->GetStringValuePtr(&Instance) = "Durin";
		EXPECT_EQ(Instance.StringValue, "Durin");

		auto* ArrayProperty = static_cast<Durin::FArrayProperty*>(Class->FindPropertyByName("ObjectArray"));
		ASSERT_NE(ArrayProperty, nullptr);
		ASSERT_NE(ArrayProperty->GetInner(), nullptr);
		EXPECT_EQ(ArrayProperty->GetContainerPtr(&Instance), &Instance.ObjectArray);
		EXPECT_EQ(ArrayProperty->GetInner()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Object);
		EXPECT_EQ(ArrayProperty->GetInner()->GetReferencedClass(), Durin::DObject::StaticClass());
		EXPECT_EQ(Class->FindPropertyByName("ObjectArray_Inner"), nullptr);

		auto* MapProperty = static_cast<Durin::FMapProperty*>(Class->FindPropertyByName("StringToInt"));
		ASSERT_NE(MapProperty, nullptr);
		ASSERT_NE(MapProperty->GetKeyProp(), nullptr);
		ASSERT_NE(MapProperty->GetValueProp(), nullptr);
		EXPECT_EQ(MapProperty->GetContainerPtr(&Instance), &Instance.StringToInt);
		EXPECT_EQ(MapProperty->GetKeyProp()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::String);
		EXPECT_EQ(MapProperty->GetValueProp()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Int32);
		EXPECT_EQ(Class->FindPropertyByName("StringToInt_Key"), nullptr);
		EXPECT_EQ(Class->FindPropertyByName("StringToInt_Value"), nullptr);

		auto* NestedArrayProperty = static_cast<Durin::FArrayProperty*>(Class->FindPropertyByName("NestedScores"));
		ASSERT_NE(NestedArrayProperty, nullptr);
		auto* NestedArrayInner = static_cast<Durin::FArrayProperty*>(NestedArrayProperty->GetInner());
		ASSERT_NE(NestedArrayInner, nullptr);
		ASSERT_NE(NestedArrayInner->GetInner(), nullptr);
		EXPECT_EQ(NestedArrayInner->GetOwnerProperty(), NestedArrayProperty);
		EXPECT_EQ(NestedArrayInner->GetInner()->GetOwnerProperty(), NestedArrayInner);
		EXPECT_EQ(NestedArrayInner->GetInner()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Int32);
		EXPECT_EQ(Class->FindPropertyByName("NestedScores_Inner"), nullptr);
		EXPECT_EQ(Class->FindPropertyByName("NestedScores_Inner_Inner"), nullptr);

		std::vector<std::string> NestedNames;
		Durin::ForEachNestedProperty(
			NestedArrayProperty,
			[&NestedNames](Durin::FProperty* Property)
			{
				NestedNames.push_back(Property->NamePrivate.ToString());
			}
		);
		ASSERT_EQ(NestedNames.size(), 2u);
		EXPECT_EQ(NestedNames[0], "NestedScores_Inner");
		EXPECT_EQ(NestedNames[1], "NestedScores_Inner_Inner");

		auto* ObjectListsProperty = static_cast<Durin::FMapProperty*>(Class->FindPropertyByName("ObjectLists"));
		ASSERT_NE(ObjectListsProperty, nullptr);
		ASSERT_NE(ObjectListsProperty->GetKeyProp(), nullptr);
		auto* ObjectListsValue = static_cast<Durin::FArrayProperty*>(ObjectListsProperty->GetValueProp());
		ASSERT_NE(ObjectListsValue, nullptr);
		ASSERT_NE(ObjectListsValue->GetInner(), nullptr);
		EXPECT_EQ(ObjectListsProperty->GetKeyProp()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::String);
		EXPECT_EQ(ObjectListsValue->GetOwnerProperty(), ObjectListsProperty);
		EXPECT_EQ(ObjectListsValue->GetInner()->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Object);
		EXPECT_EQ(ObjectListsValue->GetInner()->GetReferencedClass(), Durin::DObject::StaticClass());
	}
}
