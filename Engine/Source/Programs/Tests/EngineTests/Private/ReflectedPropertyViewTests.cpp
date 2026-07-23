#include "Editor/ReflectedPropertyView.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"

#include <gtest/gtest.h>

namespace
{
	class DPropertyViewHostTestObject final : public Durin::DObject
	{
	public:
		explicit DPropertyViewHostTestObject(Durin::DClass* Class, Durin::FName Name)
			: DObject(Class, nullptr, std::move(Name))
		{
		}

		Durin::int32 Value = 5;
	};

	class FPropertyViewRetryableRestoreAdapter final : public Durin::IReflectedPropertyMutationAdapter
	{
	public:
		auto Capture(const Durin::FReflectedPropertyEditTarget& Target,
			Durin::FPropertyValueSnapshot& OutSnapshot, std::string* OutError) const -> bool override
		{
			return Durin::GetGenericReflectedPropertyMutationAdapter().Capture(Target, OutSnapshot, OutError);
		}

		auto Apply(const Durin::FReflectedPropertyEditTarget& Target,
			const Durin::FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool override
		{
			return Durin::GetGenericReflectedPropertyMutationAdapter().Apply(Target, Snapshot, OutError);
		}

		auto Restore(const Durin::FReflectedPropertyEditTarget& Target,
			const Durin::FPropertyValueSnapshot& Snapshot, std::string* OutError) const -> bool override
		{
			if (!bAllowRestore)
			{
				if (OutError) *OutError = "Host transition restore rejected for testing.";
				return false;
			}
			return Durin::GetGenericReflectedPropertyMutationAdapter().Restore(Target, Snapshot, OutError);
		}

		bool bAllowRestore = true;
	};

	struct FPropertyViewHostTestReflection
	{
		FPropertyViewHostTestReflection()
		{
			Class = new Durin::DClass(
				Durin::EC_StaticConstructor,
				Durin::FName("DPropertyViewHostTestObject"),
				sizeof(DPropertyViewHostTestObject),
				alignof(DPropertyViewHostTestObject),
				Durin::EObjectFlags::Transient,
				Durin::EClassFlags::Native,
				Durin::EClassCastFlags::DClass,
				nullptr
			);
			DPropertyViewHostTestObject OffsetProbe(Class, Durin::FName("OffsetProbe"));
			const auto Offset = static_cast<Durin::uint16>(
				reinterpret_cast<const Durin::uint8*>(&OffsetProbe.Value)
				- reinterpret_cast<const Durin::uint8*>(&OffsetProbe)
			);
			Property = new Durin::FNumericProperty(
				Durin::FFieldVariant(Class), Durin::FName("Value"), Durin::EObjectFlags::Transient,
				Durin::EPropertyFlags::Edit, 1, Offset, sizeof(Durin::int32),
				Durin::DurinCodeGen::EPropertyGenFlags::Int32, nullptr
			);
			Property->SetValueLifecycle(sizeof(Durin::int32), alignof(Durin::int32),
				[](void* Memory) { std::construct_at(static_cast<Durin::int32*>(Memory)); },
				[](void* Memory) { std::destroy_at(static_cast<Durin::int32*>(Memory)); });
			Class->ChildProperties = Property;
			auto OwnedAdapter = std::make_unique<FPropertyViewRetryableRestoreAdapter>();
			Adapter = OwnedAdapter.get();
			EXPECT_TRUE(Durin::RegisterReflectedPropertyMutationAdapter(
				Class, Durin::FName("Value"), std::move(OwnedAdapter)));
		}

		Durin::DClass* Class = nullptr;
		Durin::FNumericProperty* Property = nullptr;
		FPropertyViewRetryableRestoreAdapter* Adapter = nullptr;
	};

	auto GetPropertyViewHostTestReflection() -> FPropertyViewHostTestReflection&
	{
		static FPropertyViewHostTestReflection Reflection;
		return Reflection;
	}

	auto BeginPropertyViewHostPreview(
		Durin::FReflectedPropertyView& View,
		const Durin::FReflectedPropertyViewContext& Context,
		DPropertyViewHostTestObject& Object
	) -> bool
	{
		FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
		if (!View.HandleOwnerContext(Context, &Object)) return false;
		return View.SubmitPropertyValueEdit(
			Context,
			Durin::FReflectedPropertyEditTarget::ForMember(&Object, Reflection.Property),
			[](Durin::FProperty* Property, void* Container, Durin::uint32 ArrayIndex) {
				*static_cast<Durin::int32*>(Property->GetValuePtr(Container, ArrayIndex)) = 8;
			},
			true
		);
	}
}

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

TEST(FReflectedPropertyViewTests, ObjectReplacementWaitsForFailedPreviewRestoration)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	DPropertyViewHostTestObject First(Reflection.Class, Durin::FName("First"));
	DPropertyViewHostTestObject Second(Reflection.Class, Durin::FName("Second"));
	Durin::FReflectedPropertyView View;
	std::string Error;
	const Durin::FReflectedPropertyViewContext Context{
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
	};
	Reflection.Adapter->bAllowRestore = true;
	EXPECT_TRUE(BeginPropertyViewHostPreview(View, Context, First));
	EXPECT_TRUE(View.IsEditingObject(&First));
	EXPECT_EQ(First.Value, 8);

	Reflection.Adapter->bAllowRestore = false;
	EXPECT_FALSE(View.HandleOwnerContext(Context, &Second));
	EXPECT_TRUE(View.IsEditingObject(&First));
	EXPECT_EQ(First.Value, 8);
	EXPECT_EQ(Error, "Host transition restore rejected for testing.");

	Reflection.Adapter->bAllowRestore = true;
	EXPECT_TRUE(View.HandleOwnerContext(Context, &Second));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(First.Value, 5);
}

TEST(FReflectedPropertyViewTests, ReadOnlyTransitionWaitsForFailedPreviewRestoration)
{
	FPropertyViewHostTestReflection& Reflection = GetPropertyViewHostTestReflection();
	DPropertyViewHostTestObject Object(Reflection.Class, Durin::FName("ReadOnly"));
	Durin::FReflectedPropertyView View;
	std::string Error;
	const Durin::FReflectedPropertyViewContext EditableContext{
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
	};
	Reflection.Adapter->bAllowRestore = true;
	EXPECT_TRUE(BeginPropertyViewHostPreview(View, EditableContext, Object));
	EXPECT_EQ(Object.Value, 8);

	Reflection.Adapter->bAllowRestore = false;
	Durin::FReflectedPropertyViewContext ReadOnlyContext = EditableContext;
	ReadOnlyContext.bReadOnly = true;
	EXPECT_FALSE(View.HandleOwnerContext(ReadOnlyContext, &Object));
	EXPECT_TRUE(View.IsEditingObject(&Object));
	EXPECT_EQ(Object.Value, 8);
	EXPECT_EQ(Error, "Host transition restore rejected for testing.");

	Reflection.Adapter->bAllowRestore = true;
	EXPECT_TRUE(View.HandleOwnerContext(ReadOnlyContext, &Object));
	EXPECT_FALSE(View.IsEditing());
	EXPECT_EQ(Object.Value, 5);
}
