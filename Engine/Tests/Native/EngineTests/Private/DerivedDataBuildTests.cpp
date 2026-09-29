#include <gtest/gtest.h>
#include "DerivedDataBuildDefinition.h"
#include <limits>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto Function() -> FBuildFunctionDescriptor
	{
		return {"Fixture.Copy", 1, 1, "Fixture.Bytes", 1,
			FCacheBucket::FromString("BuildFixture/Objects")};
	}
	auto Input(FXxHash128 Identity = FXxHash128::HashBuffer("source")) -> FBuildInputReference
	{
		return {"Source", Identity, "Fixture.Content", 1, "Fixture.Bytes", 1};
	}
	auto Action(FBuildFunctionDescriptor Descriptor, std::vector<FBuildConstant> Constants,
		std::vector<FBuildInputReference> Inputs) -> std::expected<FBuildAction, FBuildDefinitionError>
	{
		FBuildDefinitionBuilder DefinitionBuilder(Descriptor.Name);
		for (auto& Constant : Constants) std::visit([&](auto&& Value) { DefinitionBuilder.AddConstant(Constant.Name, std::forward<decltype(Value)>(Value)); }, std::move(Constant.Value));
		for (const auto& Input : Inputs) DefinitionBuilder.AddInput(Input.Name, "Capture");
		auto Definition = std::move(DefinitionBuilder).Build();
		if (!Definition) return std::unexpected(Definition.error());
		FBuildActionBuilder ActionBuilder(*Definition, std::move(Descriptor));
		for (auto& Input : Inputs) ActionBuilder.AddInput(std::move(Input));
		return std::move(ActionBuilder).Build();
	}
	auto Definition() -> FBuildAction { return Action(Function(), {{"Enabled", true}}, {Input()}).value(); }
}

TEST(FBuildDefinitionTests, CanonicalOrderingTypesAndNormalizedFloats)
{
	auto A = Action(Function(), {{"Z", uint64{9}}, {"A", -0.0f}}, {Input()});
	auto B = Action(Function(), {{"A", 0.0f}, {"Z", uint64{9}}}, {Input()});
	ASSERT_TRUE(A); ASSERT_TRUE(B);
	EXPECT_EQ(A->GetKey(), B->GetKey());
	EXPECT_TRUE(std::ranges::equal(A->GetCanonicalBytes(), B->GetCanonicalBytes()));
	EXPECT_EQ(A->GetConstants()[0].Name, "A");
	auto DifferentType = Action(Function(), {{"A", uint64{0}}, {"Z", uint64{9}}}, {Input()});
	ASSERT_TRUE(DifferentType);
	EXPECT_NE(A->GetKey(), DifferentType->GetKey());
	// Integer bytes use little endian, regardless of host ABI.
	auto Integer = Action(Function(), {{"Value", uint64{0x0102030405060708}}}, {});
	ASSERT_TRUE(Integer);
	const auto Bytes = Integer->GetCanonicalBytes();
	ASSERT_GE(Bytes.size(), 12u);
	for (size_t Index = 0; Index < 8; ++Index)
		EXPECT_EQ(Bytes[Bytes.size() - 12 + Index], static_cast<std::byte>(8 - Index));
}

TEST(FBuildDefinitionTests, EveryFunctionAndInputIdentityFieldInvalidatesTheKey)
{
	const auto Original = Definition();
	for (uint32 Field = 0; Field < 6; ++Field)
	{
		auto Changed = Function();
		switch (Field)
		{
		case 0: Changed.Name += "Other"; break;
		case 1: ++Changed.Version; break;
		case 2: ++Changed.ConstantsSchema; break;
		case 3: Changed.OutputType += "Other"; break;
		case 4: ++Changed.OutputSchema; break;
		case 5: Changed.Bucket = FCacheBucket::FromString("BuildFixture/Other"); break;
		}
		auto Built = Action(Changed, {{"Enabled", true}}, {Input()});
		ASSERT_TRUE(Built);
		EXPECT_NE(Built->GetKey(), Original.GetKey());
	}
	for (uint32 Field = 0; Field < 6; ++Field)
	{
		auto Changed = Input();
		switch (Field)
		{
		case 0: Changed.Name += "Other"; break;
		case 1: ++Changed.Identity.HashLow; break;
		case 2: Changed.IdentityScheme += "Other"; break;
		case 3: ++Changed.IdentityVersion; break;
		case 4: Changed.Representation += "Other"; break;
		case 5: ++Changed.RepresentationVersion; break;
		}
		auto Built = Action(Function(), {{"Enabled", true}}, {Changed});
		ASSERT_TRUE(Built);
		EXPECT_NE(Built->GetKey(), Original.GetKey());
	}
	auto ChangedConstant = Action(Function(), {{"Enabled", false}}, {Input()});
	ASSERT_TRUE(ChangedConstant);
	EXPECT_NE(ChangedConstant->GetKey(), Original.GetKey());
}

TEST(FBuildDefinitionTests, RejectsAmbiguousUnboundedAndNonFiniteDefinitions)
{
	EXPECT_FALSE(Action({}, {}, {}));
	EXPECT_FALSE(Action(Function(), {{"A", true}, {"A", false}}, {}));
	EXPECT_FALSE(Action(Function(), {}, {Input(), Input()}));
	EXPECT_FALSE(Action(Function(), {}, {Input({})}));
	EXPECT_FALSE(Action(Function(), {{"../path", true}}, {}));
	EXPECT_FALSE(Action(Function(), {{"A", std::numeric_limits<float>::infinity()}}, {}));
	EXPECT_FALSE(Action(Function(), {{"A", std::numeric_limits<float>::quiet_NaN()}}, {}));
	EXPECT_FALSE(Action(Function(), {{"A", std::string(1024 * 1024 + 1, 'x')}}, {}));
}

TEST(FBuildDefinitionTests, LargeUnorderedActionsCanonicalizeAndRejectDuplicateBindings)
{
	std::vector<FBuildInputReference> Inputs;
	for (size_t Index = 0; Index < 4096; ++Index)
	{
		auto Value = Input(); Value.Name = std::format("Source{:04}", Index);
		Inputs.push_back(std::move(Value));
	}
	auto First = Action(Function(), {}, Inputs); ASSERT_TRUE(First);
	std::ranges::reverse(Inputs);
	auto Reordered = Action(Function(), {}, Inputs); ASSERT_TRUE(Reordered);
	EXPECT_EQ(First->GetKey(), Reordered->GetKey());
	Inputs[20].RepresentationVersion++;
	auto Changed = Action(Function(), {}, Inputs); ASSERT_TRUE(Changed);
	EXPECT_NE(First->GetKey(), Changed->GetKey());
	Inputs[20] = Inputs[21];
	EXPECT_FALSE(Action(Function(), {}, Inputs));
}
