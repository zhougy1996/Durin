#include "DerivedDataBuildFunction.h"
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
		auto Build(FBuildContext& Context) const -> std::expected<FBuildOutput, FBuildError> override
		{
			if (Context.IsCancelled())
				return std::unexpected(FBuildError{.Phase = EBuildSessionPhase::Build, .Category = EBuildErrorCategory::Cancelled});
			if (Context.GetInputs().size() != 1 || Context.GetInputs()[0].Values.size() != 1)
				return std::unexpected(FBuildError{.Phase = EBuildSessionPhase::Build, .Description = "Missing fixture block"});
			auto Output = FBuildOutput::TryCreate({.Schema = Context.GetAction().GetFunction().OutputType,
				.SchemaVersion = Context.GetAction().GetFunction().OutputSchema, .Values = Context.GetInputs()[0].Values});
			if (!Output) return std::unexpected(FBuildError{.Phase = EBuildSessionPhase::Build, .Description = Output.error()});
			return std::move(*Output);
		}
		auto Validate(const FBuildAction& Action, const FBuildOutput& Output, const FBuildCancellation& Cancel) const
			-> std::expected<void, FBuildError> override
		{
			if (Cancel.IsCancelled())
				return std::unexpected(FBuildError{.Phase = EBuildSessionPhase::Validate, .Category = EBuildErrorCategory::Cancelled});
			if (Output.GetSchema() != Action.GetFunction().OutputType || !Output.FindValue("Data"))
				return std::unexpected(FBuildError{.Phase = EBuildSessionPhase::Validate, .Category = EBuildErrorCategory::InvalidOutput});
			return {};
		}
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

TEST(FBuildRegistryTests, RejectsDuplicateInvalidAndPostFreezeRegistration)
{
	FBuildRegistry Registry;
	EXPECT_FALSE(Registry.Register({}));
	auto Function = std::make_shared<FCopyFunction>();
	ASSERT_TRUE(Registry.Register(Function));
	EXPECT_FALSE(Registry.Register(Function));
	auto Conflicting = std::make_shared<FCopyFunction>();
	++Conflicting->DescriptorValue.Version;
	EXPECT_FALSE(Registry.Register(Conflicting));
	auto Invalid = std::make_shared<FCopyFunction>();
	Invalid->DescriptorValue.Name = "Other"; Invalid->DescriptorValue.OutputSchema = 0;
	EXPECT_FALSE(Registry.Register(Invalid));
	auto Snapshot = Registry.Freeze(); ASSERT_TRUE(Snapshot);
	EXPECT_NE(Snapshot->Find("Fixture.Copy"), nullptr);
	EXPECT_EQ(Snapshot->Find("Missing"), nullptr);
	Invalid->DescriptorValue.OutputSchema = 1;
	EXPECT_FALSE(Registry.Register(Invalid));
	EXPECT_TRUE(Registry.Freeze());
	Function->DescriptorValue.Version = 99;
	EXPECT_EQ(Snapshot->Find("Fixture.Copy")->Descriptor.Version, 1u);
}

TEST(FBuildRegistryTests, RetainedEntryOutlivesRegistryAndSharesContextBlocks)
{
	std::shared_ptr<const FRegisteredBuildFunction> Entry;
	std::weak_ptr<const IBuildFunction> Lifetime;
	{
		FBuildRegistry Registry;
		auto Function = std::make_shared<FCopyFunction>(); Lifetime = Function;
		ASSERT_TRUE(Registry.Register(Function));
		auto Snapshot = Registry.Freeze(); ASSERT_TRUE(Snapshot);
		Entry = Snapshot->Find("Fixture.Copy");
	}
	EXPECT_FALSE(Lifetime.expired());
	auto Definition = FBuildDefinition::TryCreate("Fixture.Copy", {}, {{"Source", "Capture"}});
	ASSERT_TRUE(Definition);
	auto Action = FBuildAction::TryCreate(*Definition, Entry->Descriptor, {Identity()});
	ASSERT_TRUE(Action);
	std::vector<FBuildInput> Inputs{{.Identity = Identity(), .Values = {{"Data", FSharedByteBuffer::Take(FByteBuffer(17, std::byte{1}))}}}};
	const auto* Address = Inputs[0].Values[0].Data.data();
	FBuildContext Context(*Action, Inputs, {});
	auto Output = Entry->Function->Build(Context);
	ASSERT_TRUE(Output);
	EXPECT_TRUE(Entry->Function->Validate(*Action, *Output, {}));
	FBuildCancellation Cancel([] { return true; });
	EXPECT_FALSE(Entry->Function->Validate(*Action, *Output, Cancel));
	FBuildContext Cancelled(*Action, Inputs, Cancel);
	auto Failure = Entry->Function->Build(Cancelled);
	ASSERT_FALSE(Failure); EXPECT_EQ(Failure.error().Category, EBuildErrorCategory::Cancelled);
	Inputs.clear(); Entry.reset();
	EXPECT_TRUE(Lifetime.expired());
	EXPECT_EQ(Output->FindValue("Data")->Data.data(), Address);
}

TEST(FBuildRegistryTests, BoundsDiagnosticTextWithoutDiscardingProducerIdentity)
{
	FBuildError Error{.Phase = EBuildSessionPhase::Build, .Category = EBuildErrorCategory::ProducerFailure,
		.Description = std::string(5000, 'x'), .ProducerCode = 42, .DiagnosticIdentity = FXxHash128::HashBuffer("semantic")};
	Error.BoundDescription();
	EXPECT_EQ(Error.Description.size(), 4096u);
	EXPECT_EQ(Error.ProducerCode, 42u);
	EXPECT_EQ(Error.DiagnosticIdentity, FXxHash128::HashBuffer("semantic"));
}
