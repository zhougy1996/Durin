#include "DerivedDataBuildSession.h"
#include "Serialization/BinaryFormat.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	auto Descriptor() -> FBuildFunctionDescriptor
	{
		return {"Fixture.Copy", 1, 1, "Fixture.Output", 1, FCacheBucket::FromString("SessionFixture")};
	}
	auto Identity(std::string Name = "Source") -> FBuildInputReference
	{
		return {std::move(Name), FXxHash128::HashBuffer("captured"), "Fixture.Semantic", 1, "Fixture.Bytes", 1};
	}
	template<typename T> concept CHasKey = requires(const T& Value) { Value.GetKey(); };
	static_assert(!CHasKey<FBuildDefinition> && CHasKey<FBuildAction>);

	class FCopyFunction final : public IBuildFunction
	{
	public:
		FBuildFunctionDescriptor DescriptorValue = Descriptor();
		auto GetDescriptor() const -> FBuildFunctionDescriptor override { return DescriptorValue; }
		auto Build(FBuildContext& Context) const -> std::expected<FBuildOutput, FBuildFailure> override
		{
			if (Context.IsCancelled())
				return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InternalFailure, .Operation = EBuildOperation::Build});
			if (Context.GetInputs().size() != 1 || Context.GetInputs()[0].Values.size() != 1)
				return std::unexpected(FBuildFailure{.Operation = EBuildOperation::Build, .Description = "Missing fixture block"});
			auto Output = FBuildOutput::TryCreate({.Schema = Context.GetAction().GetFunction().OutputType,
				.SchemaVersion = Context.GetAction().GetFunction().OutputSchema, .Values = Context.GetInputs()[0].Values});
			if (!Output) return std::unexpected(FBuildFailure{.Operation = EBuildOperation::Build, .Description = Output.error()});
			return std::move(*Output);
		}
		auto Validate(const FBuildAction& Action, const FBuildOutput& Output, const FBuildCancellation& Cancel) const
			-> FBuildValidationResult override
		{
			if (Cancel.IsCancelled())
				return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InternalFailure, .Operation = EBuildOperation::Validate});
			if (Output.GetSchema() != Action.GetFunction().OutputType || !Output.FindValue("Data"))
				return std::unexpected(FBuildFailure{.Reason = EBuildFailureReason::InvalidOutput, .Operation = EBuildOperation::Validate});
			return {};
		}
	};
	class FCopyResolver final : public IBuildInputResolver
	{
	public:
		FSharedByteBuffer Data = FSharedByteBuffer::Take(FByteBuffer(17, std::byte{1}));
		auto Describe(std::span<const FBuildSourceReference>, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInputReference>, FBuildFailure> override
		{ return std::vector{Identity()}; }
		auto Resolve(std::span<const FBuildInputReference>, const FBuildCancellation&) const
			-> std::expected<std::vector<FBuildInput>, FBuildFailure> override
		{ return std::vector<FBuildInput>{{.Identity = Identity(), .Values = {{"Data", Data}}}}; }
	};
}

TEST(FBuildActionTests, FreezesRegisteredVersionsAndResolvedIdentitiesWithoutSourceLocators)
{
	auto A = FBuildDefinition::TryCreate("Fixture.Copy", {{"Z", uint64{9}}, {"A", -0.0f}}, {{"Source", "CaptureA"}});
	auto B = FBuildDefinition::TryCreate("Fixture.Copy", {{"A", 0.0f}, {"Z", uint64{9}}}, {{"Source", "CaptureB"}});
	ASSERT_TRUE(A); ASSERT_TRUE(B);
	auto First = FBuildAction::TryCreate(*A, Descriptor(), {Identity()});
	auto Second = FBuildAction::TryCreate(*B, Descriptor(), {Identity()});
	ASSERT_TRUE(First); ASSERT_TRUE(Second);
	EXPECT_EQ(First->GetKey(), Second->GetKey());
	EXPECT_TRUE(std::ranges::equal(First->GetCanonicalBytes(), Second->GetCanonicalBytes()));
	FBinaryReader Reader(First->GetCanonicalBytes());
	std::string Tag;
	uint32 Schema = 0;
	ASSERT_TRUE(Reader.ReadString(Tag)); ASSERT_TRUE(Reader.ReadU32(Schema));
	EXPECT_EQ(Tag, "Durin.DerivedData.BuildAction"); EXPECT_EQ(Schema, 2u);
	EXPECT_EQ(First->GetKey().ToString(), "5046b215c7c0a6e98d4bd4a0c5c70264");
	auto Version = Descriptor(); ++Version.Version;
	auto Updated = FBuildAction::TryCreate(*A, Version, {Identity()});
	ASSERT_TRUE(Updated); EXPECT_NE(First->GetKey(), Updated->GetKey());
	EXPECT_EQ(First->GetFunction().Version, 1u);
	auto ChangedIdentity = Identity(); ++ChangedIdentity.RepresentationVersion;
	Updated = FBuildAction::TryCreate(*A, Descriptor(), {ChangedIdentity});
	ASSERT_TRUE(Updated); EXPECT_NE(First->GetKey(), Updated->GetKey());
}

TEST(FBuildActionTests, RejectsInvalidDefinitionsDescriptorsAndMetadataBindings)
{
	EXPECT_FALSE(FBuildDefinition::TryCreate("", {}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate("Fixture.Copy", {{"A", true}, {"A", false}}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate("Fixture.Copy", {{"A", std::numeric_limits<float>::infinity()}}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate("Fixture.Copy", {{"A", std::string(1024 * 1024 + 1, 'x')}}, {}));
	EXPECT_FALSE(FBuildDefinition::TryCreate("Fixture.Copy", {}, {{"Source", ""}}));
	EXPECT_FALSE(FBuildDefinition::TryCreate("Fixture.Copy", {}, {{"Source", "A"}, {"Source", "B"}}));
	auto Definition = FBuildDefinition::TryCreate("Fixture.Copy", {}, {{"Source", "Captured"}});
	ASSERT_TRUE(Definition);
	EXPECT_FALSE(FBuildAction::TryCreate(*Definition, Descriptor(), {}));
	EXPECT_FALSE(FBuildAction::TryCreate(*Definition, Descriptor(), {Identity("Unknown")}));
	EXPECT_FALSE(FBuildAction::TryCreate(*Definition, Descriptor(), {Identity(), Identity()}));
	auto BadInput = Identity(); BadInput.Identity = {};
	EXPECT_FALSE(FBuildAction::TryCreate(*Definition, Descriptor(), {BadInput}));
	auto BadFunction = Descriptor(); BadFunction.Name = "Other";
	EXPECT_FALSE(FBuildAction::TryCreate(*Definition, BadFunction, {Identity()}));
	BadFunction = Descriptor(); BadFunction.Version = 0;
	EXPECT_FALSE(FBuildAction::TryCreate(*Definition, BadFunction, {Identity()}));
}

TEST(FBuildServiceTests, OwnsRegistrationAndFreezesOnFirstSession)
{
	auto Service = CreateBuild();
	EXPECT_FALSE(Service->Register({}));
	auto Function = std::make_shared<FCopyFunction>();
	ASSERT_TRUE(Service->Register(Function));
	EXPECT_FALSE(Service->Register(Function));
	auto Conflicting = std::make_shared<FCopyFunction>();
	++Conflicting->DescriptorValue.Version;
	EXPECT_FALSE(Service->Register(Conflicting));
	auto Invalid = std::make_shared<FCopyFunction>();
	Invalid->DescriptorValue.Name = "Other"; Invalid->DescriptorValue.OutputSchema = 0;
	EXPECT_FALSE(Service->Register(Invalid));
	ASSERT_TRUE(Service->CreateSession());
	Invalid->DescriptorValue.OutputSchema = 1;
	EXPECT_FALSE(Service->Register(Invalid));
	Function->DescriptorValue.Version = 99;
}

TEST(FBuildServiceTests, RetainedFunctionAndOutputBlocksOutliveDrainedService)
{
	std::weak_ptr<const IBuildFunction> Lifetime;
	auto Service = CreateBuild();
	auto Function = std::make_shared<FCopyFunction>(); Lifetime = Function;
	ASSERT_TRUE(Service->Register(Function));
	auto Session = Service->CreateSession().value();
	auto Resolver = std::make_shared<FCopyResolver>();
	const auto* Address = Resolver->Data.data();
	EXPECT_FALSE(Lifetime.expired());
	auto Definition = FBuildDefinition::TryCreate("Fixture.Copy", {}, {{"Source", "Capture"}});
	ASSERT_TRUE(Definition);
	auto Inputs = FBuildInputs::TryCreate(Definition->GetSources(), Resolver).value();
	std::optional<FBuildCompleteParams> Completion;
	ASSERT_TRUE(Session->Build(std::move(*Definition), [&](auto Value) { Completion = std::move(Value); },
		std::move(Inputs), {.Policy = {.QueryCache = false, .StoreOnBuild = false}}));
	ASSERT_TRUE(Completion); ASSERT_EQ(Completion->GetStatus(), EStatus::Ok);
	FBuildOutput Output = *Completion->GetOutput();
	Function.reset(); Resolver.reset(); Completion.reset();
	Service->Close(); EXPECT_EQ(Service->Drain(), EBuildDrainResult::Drained);
	Session.reset(); Service.reset();
	EXPECT_TRUE(Lifetime.expired());
	EXPECT_EQ(Output.FindValue("Data")->Data.data(), Address);
}

TEST(FBuildServiceTests, BoundsDiagnosticTextWithoutDiscardingProducerIdentity)
{
	FBuildFailure Error{.Reason = EBuildFailureReason::ProducerFailure, .Operation = EBuildOperation::Build,
		.Description = std::string(5000, 'x'), .ProducerCode = 42, .DiagnosticIdentity = FXxHash128::HashBuffer("semantic")};
	Error.BoundDescription();
	EXPECT_EQ(Error.Description.size(), 4096u);
	EXPECT_EQ(Error.ProducerCode, 42u);
	EXPECT_EQ(Error.DiagnosticIdentity, FXxHash128::HashBuffer("semantic"));
}

TEST(FBuildServiceTests, RegistrationExceptionsBecomeTypedAdmissionFailures)
{
	struct FThrowingFunction final : IBuildFunction
	{
		auto GetDescriptor() const -> FBuildFunctionDescriptor override
		{ throw std::runtime_error("descriptor exception"); }
		auto Build(FBuildContext&) const -> FBuildFunctionResult override
		{ return std::unexpected(FBuildFailure{}); }
		auto Validate(const FBuildAction&, const FBuildOutput&, const FBuildCancellation&) const
			-> FBuildValidationResult override { return {}; }
	};
	auto Service = CreateBuild();
	auto Registered = Service->Register(std::make_shared<FThrowingFunction>());
	ASSERT_FALSE(Registered);
	EXPECT_EQ(Registered.error().Reason, EBuildAdmissionReason::InternalFailure);
	EXPECT_EQ(Registered.error().Description, "descriptor exception");
}
