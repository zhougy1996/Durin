#include "DerivedDataBuildSession.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto Identity() -> FBuildInputReference
	{ return {"Source", FXxHash128::HashBuffer("source"), "Fixture", 1, "Bytes", 1}; }
	struct FResolver final : IBuildInputResolver
	{
		auto Describe(std::span<const FBuildSourceReference>, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildInputError> override { return std::vector{Identity()}; }
		auto Resolve(std::span<const FBuildInputReference>, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInput>, FBuildInputError> override
		{ return std::vector<FBuildInput>{{.Identity = Identity(), .Values = {{"Data", FSharedByteBuffer::Take(FByteBuffer(8, std::byte{7}))}}}}; }
	};
	struct FCopy final : IBuildFunction
	{
		bool Error = false;
		auto GetName() const -> std::string_view override { return "Fixture.Copy"; }
		auto GetVersion() const -> uint32 override { return 1; }
		auto Configure(FBuildConfigContext& C) const -> void override
		{ C.SetConstantsSchema(1); C.SetOutput("Fixture.Output", 1); C.SetCacheBucket(FCacheBucket::FromString("Fixture")); }
		auto Build(FBuildContext& C) const -> void override
		{
			if (Error) return C.AddError("deterministic failure"), void();
			const auto* Input = C.FindInput("Source");
			if (!Input || Input->Values.size() != 1) return C.AddError("missing input"), void();
			C.AddValue(FValueId::FromName("Data"), Input->Values[0].Data);
			C.AddWarning("fixture warning");
		}
	};
	auto Definition(bool Enabled = true) -> FBuildDefinition
	{
		FBuildDefinitionBuilder B("Fixture.Copy"); B.AddConstant("Enabled", Enabled).AddInput("Source", "Capture"); return std::move(B).Build().value();
	}
}

TEST(FBuildFunctionTests, BuilderCanonicalIdentityMatchesLegacySchemaTwoEncoding)
{
	FBuildDefinitionBuilder A("Fixture.Copy"), B("Fixture.Copy");
	A.AddConstant("Z", uint64{9}).AddConstant("A", -0.0f).AddInput("Source", "Capture");
	B.AddConstant("A", 0.0f).AddConstant("Z", uint64{9}).AddInput("Source", "Capture");
	auto DA = std::move(A).Build(); auto DB = std::move(B).Build(); ASSERT_TRUE(DA); ASSERT_TRUE(DB);
	FBuildFunctionDescriptor D{"Fixture.Copy", 1, 1, "Fixture.Output", 1, FCacheBucket::FromString("Fixture")};
	FBuildActionBuilder BA(*DA, D), BB(*DB, D); BA.AddInput(Identity()); BB.AddInput(Identity());
	auto AA = std::move(BA).Build(); auto AB = std::move(BB).Build(); ASSERT_TRUE(AA); ASSERT_TRUE(AB);
	EXPECT_EQ(AA->GetKey(), AB->GetKey()); EXPECT_TRUE(std::ranges::equal(AA->GetCanonicalBytes(), AB->GetCanonicalBytes()));
}

TEST(FBuildFunctionTests, BuildersRejectDuplicatesAndInvalidConstants)
{
	FBuildDefinitionBuilder Duplicate("Fixture.Copy"); Duplicate.AddConstant("A", true).AddConstant("A", false); EXPECT_FALSE(std::move(Duplicate).Build());
	FBuildDefinitionBuilder Invalid("Fixture.Copy"); Invalid.AddConstant("A", std::numeric_limits<float>::infinity()); EXPECT_FALSE(std::move(Invalid).Build());
	FBuildDefinitionBuilder Inputs("Fixture.Copy"); Inputs.AddInput("Source", "A").AddInput("Source", "B"); EXPECT_FALSE(std::move(Inputs).Build());
}

TEST(FBuildFunctionTests, ContextPublishesValueWarningAndDeterministicError)
{
	auto Function = std::make_shared<FCopy>(); auto Resolver = std::make_shared<FResolver>(); auto Service = CreateBuild();
	ASSERT_TRUE(Service->Register(Function)); auto Session = Service->CreateSession(); ASSERT_TRUE(Session);
	auto DefinitionValue = Definition(); auto Inputs = std::move(FBuildInputsBuilder(DefinitionValue.GetSources(), Resolver)).Build(); ASSERT_TRUE(Inputs);
	std::optional<FBuildCompleteParams> Result;
	FBuildRequestOptions Options{.Policy = {.QueryCache = false, .StoreOnBuild = false}};
	ASSERT_TRUE((*Session)->Build(DefinitionValue, [&](auto Value) { Result = std::move(Value); }, std::move(*Inputs), Options));
	ASSERT_TRUE(Result); EXPECT_EQ(Result->GetStatus(), EStatus::Ok); ASSERT_NE(Result->GetOutput(), nullptr);
	EXPECT_NE(Result->GetOutput()->FindValue(FValueId::FromName("Data")), nullptr); ASSERT_EQ(Result->GetOutput()->GetMessages().size(), 1u);
	Function->Error = true; Inputs = std::move(FBuildInputsBuilder(DefinitionValue.GetSources(), Resolver)).Build(); Result.reset();
	ASSERT_TRUE((*Session)->Build(DefinitionValue, [&](auto Value) { Result = std::move(Value); }, std::move(*Inputs), Options));
	ASSERT_TRUE(Result); EXPECT_EQ(Result->GetStatus(), EStatus::Error); ASSERT_NE(Result->GetOutput(), nullptr);
	EXPECT_TRUE(Result->GetOutput()->HasError()); EXPECT_TRUE(Result->GetOutput()->GetValues().empty());
}
