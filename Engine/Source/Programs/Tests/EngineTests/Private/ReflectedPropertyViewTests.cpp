#include "Editor/ReflectedPropertyView.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"

#include <gtest/gtest.h>

TEST(FReflectedPropertyViewTests, HidesConventionalBoolPrefixFromDisplayName)
{
	using Durin::DurinCodeGen::EPropertyGenFlags;

	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::Bool), "Simulate Physics");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("bUseHDR", EPropertyGenFlags::Bool), "Use HDR");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("border", EPropertyGenFlags::Bool), "border");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("b", EPropertyGenFlags::Bool), "b");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("GroundHeight", EPropertyGenFlags::Float), "Ground Height");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("URLValue", EPropertyGenFlags::String), "URL Value");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::String), "b Simulate Physics");
	EXPECT_EQ(Durin::MakeReflectedPropertyDisplayName("bSimulatePhysics", EPropertyGenFlags::Bool, "Simulate Physics"), "Simulate Physics");
}

TEST(FReflectedPropertyViewTests, EditObjectEnumeratesEditableStaticArrayElementsBeforeSearch)
{
	using namespace Durin;
	using DurinCodeGen::EPropertyGenFlags;

	DClass TestClass(
		EC_StaticConstructor,
		FName("FReflectedPropertyViewTestObject"),
		sizeof(DObject),
		alignof(DObject),
		EObjectFlags::Transient,
		EClassFlags::Native,
		EClassCastFlags::DClass,
		nullptr
	);
	FNumericProperty EditableProperty(
		FFieldVariant(&TestClass), FName("TestValues"), EObjectFlags::Transient,
		EPropertyFlags::Edit, 3, 0, sizeof(float), EPropertyGenFlags::Float, nullptr
	);
	FNumericProperty HiddenProperty(
		FFieldVariant(&TestClass), FName("HiddenValues"), EObjectFlags::Transient,
		EPropertyFlags::None, 2, 0, sizeof(float), EPropertyGenFlags::Float, nullptr
	);
	EditableProperty.Next = &HiddenProperty;
	TestClass.ChildProperties = &EditableProperty;
	DObject Object(&TestClass, nullptr, FName("Object"));

	std::vector<uint32> FilteredIndices;
	FReflectedPropertyView View;
	const FObjectPropertyViewResult Result = View.EditObject({}, &Object, {
		.SearchText = "not present",
		.Filter = [&](const FProperty& Property, uint32 ArrayIndex) {
			EXPECT_EQ(&Property, &EditableProperty);
			FilteredIndices.push_back(ArrayIndex);
			return true;
		},
		.bCreatePropertyTable = false,
		.bShowEmptyMessage = false,
	});

	EXPECT_EQ(FilteredIndices, (std::vector<uint32>{0, 1, 2}));
	EXPECT_EQ(Result.VisiblePropertyCount, 0u);
	EXPECT_FALSE(Result.bChanged);
	EXPECT_EQ(MakeReflectedPropertyLabel(EditableProperty, 2), "Test Values[2]");
}
