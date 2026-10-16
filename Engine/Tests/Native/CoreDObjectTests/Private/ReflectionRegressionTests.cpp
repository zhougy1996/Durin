#include "CoreMinimal.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/FunctionParameters.h"
#include "DObject/ObjectKey.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "FunctionReflectionTestTypes.h"
#include "NativeDObjectTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::FunctionFixtures;
}

TEST(FCoreDObjectReflectionRegressionTests, FieldClassInheritanceIncludesEveryAncestor)
{
	Testing::InitializeDObjectSystemForTests();
	const auto* Numeric = FNumericProperty::StaticClass();
	EXPECT_TRUE(Numeric->IsChildOf(Numeric));
	EXPECT_TRUE(Numeric->IsChildOf(FProperty::StaticClass()));
	EXPECT_TRUE(Numeric->IsChildOf(FField::StaticClass()));
	EXPECT_FALSE(FProperty::StaticClass()->IsChildOf(Numeric));
	EXPECT_FALSE(Numeric->IsChildOf(FStringProperty::StaticClass()));
}

TEST(FCoreDObjectReflectionRegressionTests, FieldClassNameRegistryRetainsRegistration)
{
	Testing::InitializeDObjectSystemForTests();
	auto* Numeric = FNumericProperty::StaticClass();
	auto* String = FStringProperty::StaticClass();
	const auto& Types = FFieldClass::GetNameToFieldClassMap();
	ASSERT_TRUE(Types.contains("NumericProperty"));
	ASSERT_TRUE(Types.contains("StringProperty"));
	EXPECT_EQ(Types.at("NumericProperty"), Numeric);
	EXPECT_EQ(Types.at("StringProperty"), String);
}

TEST(FCoreDObjectReflectionRegressionTests, GeneratedPropertiesBeyond64KiBAddressNativeMembersAndRetainReferences)
{
	Testing::InitializeDObjectSystemForTests();
	TStrongObjectPtr<DLargePropertyTarget> Target(NewObject<DLargePropertyTarget>(nullptr, "LargePropertyTarget"));
	ASSERT_TRUE(Target);
	auto* Value = Target->GetClass()->FindPropertyByName("Value");
	auto* Text = Target->GetClass()->FindPropertyByName("Text");
	auto* Reference = Target->GetClass()->FindPropertyByName("Reference");
	ASSERT_NE(Value, nullptr);
	ASSERT_NE(Text, nullptr);
	ASSERT_NE(Reference, nullptr);
	for (const auto* Property : {Value, Text, Reference}) EXPECT_GT(Property->GetOffset(), 65535u);
	ASSERT_EQ(Value->GetValuePtr(Target.Get()), &Target->Value);
	ASSERT_EQ(Text->GetValuePtr(Target.Get()), &Target->Text);
	ASSERT_EQ(Reference->GetValuePtr(Target.Get()), &Target->Reference);
	*Value->ContainerPtrToValuePtr<int32>(Target.Get()) = 42;
	*Text->ContainerPtrToValuePtr<std::string>(Target.Get()) = "updated";
	EXPECT_EQ(Target->Value, 42);
	EXPECT_EQ(Target->Text, "updated");
	EXPECT_TRUE(std::ranges::all_of(Target->Padding, [](uint8 Byte) { return Byte == 0; }));

	// Handwritten native parameter records share the widened property offset ABI.
	DurinCodeGen::TNativePropertyParams<int32> NativeValue("Value", Value->GetOffset());
	auto NativeProperty = DurinCodeGen::ConstructProperty(FFieldVariant{}, NativeValue.Params);
	ASSERT_EQ(NativeProperty->GetValuePtr(Target.Get()), &Target->Value);

	auto* Object = NewObject<DObject>(nullptr, "LargePropertyReference");
	const FObjectKey Key(Object);
	Target->Reference = Object;
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), Object);
	Target->Reference = nullptr;
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), nullptr);
	Target.Reset();
	CollectGarbage();
}

TEST(FCoreDObjectReflectionRegressionTests, OverAlignedObjectsUseMatchingAllocationAndVirtualDeletion)
{
	Testing::InitializeDObjectSystemForTests();
	static_assert(alignof(DAlignedAllocationTarget) > __STDCPP_DEFAULT_NEW_ALIGNMENT__);
	const auto Destroyed = DAlignedAllocationTarget::Destroyed;
	const auto Deallocated = DAlignedAllocationTarget::Deallocated;
	DAlignedAllocationTarget::bThrowOnConstruction = false;
	auto* Object = NewObject<DAlignedAllocationTarget>(nullptr, "AlignedAllocationTarget");
	ASSERT_NE(Object, nullptr);
	EXPECT_EQ(Object->GetClass()->GetMinAlignment(), alignof(DAlignedAllocationTarget));
	EXPECT_TRUE(DAlignedAllocationTarget::bConstructorWasAligned);
	EXPECT_EQ(reinterpret_cast<uintptr_t>(Object) % alignof(DAlignedAllocationTarget), 0u);
	const FObjectKey Key(Object);
	MarkAsGarbage(Object);
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(Key), nullptr);
	EXPECT_EQ(DAlignedAllocationTarget::Destroyed, Destroyed + 1);
	EXPECT_EQ(DAlignedAllocationTarget::Deallocated, Deallocated + 1);
	EXPECT_EQ(DAlignedAllocationTarget::DeallocationAlignment, alignof(DAlignedAllocationTarget));
}

TEST(FCoreDObjectReflectionRegressionTests, OverAlignedConstructionFailureCleansUpAndAllowsRetry)
{
	Testing::InitializeDObjectSystemForTests();
	const auto Destroyed = DAlignedAllocationTarget::Destroyed;
	DAlignedAllocationTarget::bThrowOnConstruction = true;
	for (uint32 Attempt = 0; Attempt < 8; ++Attempt)
	{
		EXPECT_THROW(NewObject<DAlignedAllocationTarget>(nullptr, "FailedAlignedTarget"), std::runtime_error);
		EXPECT_TRUE(DAlignedAllocationTarget::bConstructorWasAligned);
	}
	DAlignedAllocationTarget::bThrowOnConstruction = false;
	EXPECT_EQ(DAlignedAllocationTarget::Destroyed, Destroyed);
	TStrongObjectPtr<DAlignedAllocationTarget> Object(NewObject<DAlignedAllocationTarget>(nullptr, "FailedAlignedTarget"));
	ASSERT_TRUE(Object);
	EXPECT_TRUE(DAlignedAllocationTarget::bConstructorWasAligned);
	Object.Reset();
	CollectGarbage();
	EXPECT_EQ(DAlignedAllocationTarget::Destroyed, Destroyed + 1);
}
