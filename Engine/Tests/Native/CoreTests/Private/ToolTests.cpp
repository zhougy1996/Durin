#include "Tools/Tool.h"
#include "Tools/NativeToolset.h"

#include <gtest/gtest.h>

namespace Durin::Tests
{
	namespace
	{
		auto MakeDescriptor() -> FToolDescriptor
		{
			FToolDescriptor Descriptor;
			Descriptor.Name = "configure";
			Descriptor.Input.Properties.emplace("enabled", FToolSchema(EToolValueType::Boolean));
			Descriptor.Input.Properties.at("enabled").Default = true;
			Descriptor.Input.Required = {"enabled"};
			Descriptor.Output = Descriptor.Input;
			return Descriptor;
		}

		auto Request(FToolValue::FObject Arguments = {}) -> FToolRequest
		{
			return {{"tests", "configure"}, 1, std::move(Arguments)};
		}

		class FTestToolset final : public IToolset
		{
		public:
			auto DescribeToolset() const -> FToolsetDescriptor override { return {"tests", "Test tools", {MakeDescriptor()}}; }
			auto CheckAvailability(const FToolRequest&, const FToolContext& Context) -> FToolAvailability override
			{
				return {!Context.Cancellation.IsCancellationRequested(), {}};
			}
			auto Call(const FToolRequest& InRequest, const FToolContext&) -> FToolResult override
			{
				return {EToolStatus::Succeeded, InRequest.Arguments};
			}
		};
	}

	TEST(FToolTests, NormalizesDefaultsWithoutChangingInputAndPreservesExplicitValues)
	{
		const auto Descriptor = MakeDescriptor();
		const auto Input = Request();
		const auto Prepared = PrepareToolArguments(Descriptor, Input);
		ASSERT_TRUE(Prepared.has_value());
		EXPECT_TRUE(std::get<FToolValue::FObject>(Input.Arguments.Data).empty());
		EXPECT_TRUE(std::get<bool>(std::get<FToolValue::FObject>(Prepared->Arguments.Data).at("enabled").Data));
		const auto Explicit = PrepareToolArguments(Descriptor, Request({{"enabled", false}}));
		ASSERT_TRUE(Explicit.has_value());
		EXPECT_FALSE(std::get<bool>(std::get<FToolValue::FObject>(Explicit->Arguments.Data).at("enabled").Data));
		EXPECT_EQ(Prepared->Arguments, PrepareToolArguments(Descriptor, *Prepared)->Arguments);
	}

	TEST(FToolTests, CategorizesNameVersionRequiredUnknownAndTypeFailures)
	{
		auto Descriptor = MakeDescriptor();
		auto WrongName = Request();
		WrongName.Address.Tool = "other";
		EXPECT_EQ(EToolError::ToolUnavailable, PrepareToolArguments(Descriptor, WrongName).error().Code);
		auto WrongVersion = Request();
		WrongVersion.ContractVersion = 0;
		EXPECT_EQ(EToolError::VersionMismatch, PrepareToolArguments(Descriptor, WrongVersion).error().Code);
		Descriptor.Input.Properties.at("enabled").Default.reset();
		const auto Missing = PrepareToolArguments(Descriptor, Request());
		ASSERT_FALSE(Missing.has_value());
		EXPECT_EQ("/enabled", Missing.error().Path);
		const auto WrongType = PrepareToolArguments(Descriptor, Request({{"enabled", "true"}}));
		ASSERT_FALSE(WrongType.has_value());
		EXPECT_EQ(EToolError::InvalidArguments, WrongType.error().Code);
		const auto Unknown = PrepareToolArguments(Descriptor, Request({{"bad~/key", true}}));
		ASSERT_FALSE(Unknown.has_value());
		EXPECT_EQ("/bad~0~1key", Unknown.error().Path);
	}

	TEST(FToolTests, CopiesNestedSchemaAndValuesWithoutBorrowedStorage)
	{
		auto Descriptor = MakeDescriptor();
		FToolSchema Array(EToolValueType::Array);
		Array.Items = std::make_unique<FToolSchema>(EToolValueType::String);
		Array.Default = FToolValue::FArray{"original"};
		Descriptor.Input.Properties.emplace("labels", std::move(Array));
		auto Copy = Descriptor;
		Copy.Input.Properties.at("labels").Items->MaxSize = 3;
		std::get<FToolValue::FArray>(Copy.Input.Properties.at("labels").Default->Data)[0] = "new";
		EXPECT_FALSE(Descriptor.Input.Properties.at("labels").Items->MaxSize.has_value());
		const auto Prepared = PrepareToolArguments(Descriptor, Request());
		ASSERT_TRUE(Prepared.has_value());
		Descriptor = {};
		EXPECT_EQ(FToolValue(FToolValue::FArray{"original"}), std::get<FToolValue::FObject>(Prepared->Arguments.Data).at("labels"));
	}

	TEST(FToolTests, ValidatesNestedArraysAndDoesNotApplyOutputDefaults)
	{
		auto Descriptor = MakeDescriptor();
		Descriptor.Output = FToolSchema(EToolValueType::Array);
		Descriptor.Output.Items = std::make_unique<FToolSchema>(Descriptor.Input);
		EXPECT_TRUE(ValidateToolOutput(Descriptor, FToolValue::FArray{FToolValue::FObject{{"enabled", true}}}));
		const auto Missing = ValidateToolOutput(Descriptor, FToolValue::FArray{FToolValue::FObject{}});
		ASSERT_FALSE(Missing.has_value());
		EXPECT_EQ(EToolError::InvalidOutput, Missing.error().Code);
		EXPECT_EQ("/0/enabled", Missing.error().Path);
	}

	TEST(FToolTests, PreservesFullWidthIntegerBoundsAndRequiresExactNumericTypes)
	{
		auto Descriptor = MakeDescriptor();
		FToolSchema Number(EToolValueType::UnsignedInteger);
		Number.Minimum = std::numeric_limits<uint64>::max() - 1;
		Number.Maximum = std::numeric_limits<uint64>::max();
		Descriptor.Input.Properties.emplace("value", Number);
		EXPECT_TRUE(PrepareToolArguments(Descriptor, Request({{"value", std::numeric_limits<uint64>::max()}})));
		EXPECT_FALSE(PrepareToolArguments(Descriptor, Request({{"value", std::numeric_limits<uint64>::max() - 2}})));
		EXPECT_FALSE(PrepareToolArguments(Descriptor, Request({{"value", int64{1}}})));
		Number = FToolSchema(EToolValueType::Integer);
		Number.Minimum = std::numeric_limits<int64>::min();
		Number.Maximum = std::numeric_limits<int64>::min() + 1;
		Descriptor.Input.Properties.at("value") = Number;
		EXPECT_TRUE(PrepareToolArguments(Descriptor, Request({{"value", std::numeric_limits<int64>::min()}})));
		EXPECT_FALSE(PrepareToolArguments(Descriptor, Request({{"value", std::numeric_limits<int64>::min() + 2}})));
	}

	TEST(FToolTests, RejectsNonFiniteNumbersAndChecksScalarEnumsAndSizes)
	{
		auto Descriptor = MakeDescriptor();
		Descriptor.Input.Properties.emplace("number", FToolSchema(EToolValueType::Number));
		for (double Value : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
			EXPECT_FALSE(PrepareToolArguments(Descriptor, Request({{"number", Value}})));
		FToolSchema Mode(EToolValueType::String);
		Mode.Enum = {"fast", "slow"};
		Mode.MinSize = 4;
		Mode.MaxSize = 4;
		Descriptor.Input.Properties.emplace("mode", Mode);
		EXPECT_TRUE(PrepareToolArguments(Descriptor, Request({{"mode", "fast"}})));
		EXPECT_FALSE(PrepareToolArguments(Descriptor, Request({{"mode", "other"}})));
		Descriptor.Input.Properties.at("number").Minimum = -1.0;
		Descriptor.Input.Properties.at("number").Maximum = 1.0;
		EXPECT_FALSE(PrepareToolArguments(Descriptor, Request({{"number", 2.0}})));
	}

	TEST(FToolTests, RejectsUnsupportedAndInconsistentSchemaMetadata)
	{
		const auto CheckInvalid = [](FToolSchema Schema) {
			auto Descriptor = MakeDescriptor();
			Descriptor.Output = std::move(Schema);
			const auto Result = ValidateToolDescriptor(Descriptor);
			ASSERT_FALSE(Result.has_value());
			EXPECT_EQ(EToolError::InvalidSchema, Result.error().Code);
		};
		CheckInvalid(FToolSchema(static_cast<EToolValueType>(255)));
		CheckInvalid(FToolSchema(EToolValueType::Array));
		FToolSchema Schema(EToolValueType::Boolean);
		Schema.Minimum = false;
		CheckInvalid(Schema);
		Schema = FToolSchema(EToolValueType::String);
		Schema.Enum = {"same", "same"};
		CheckInvalid(Schema);
		Schema.Enum = {true};
		CheckInvalid(Schema);
		Schema.Enum.clear();
		Schema.MinSize = 3;
		Schema.MaxSize = 2;
		CheckInvalid(Schema);
		Schema = FToolSchema(EToolValueType::Integer);
		Schema.Minimum = int64{2};
		Schema.Maximum = int64{1};
		CheckInvalid(Schema);
		Schema = FToolSchema(EToolValueType::Object);
		Schema.Required = {"missing"};
		CheckInvalid(Schema);
		Schema.Properties.emplace("missing", FToolSchema());
		Schema.Required.push_back("missing");
		CheckInvalid(Schema);
		Schema = FToolSchema(EToolValueType::Boolean);
		Schema.Default = "not boolean";
		CheckInvalid(Schema);
	}

	TEST(FToolTests, RejectsInvalidDescriptorsAndNeverCoercesNames)
	{
		for (const char* Name : {"", "Tests.configure", "tests..configure", ".tests", "tests.", "tests.bad-name", "tests.1bad"})
		{
			auto Descriptor = MakeDescriptor();
			Descriptor.Name = Name;
			EXPECT_EQ(EToolError::InvalidDescriptor, ValidateToolDescriptor(Descriptor).error().Code);
		}
		auto Descriptor = MakeDescriptor();
		Descriptor.ContractVersion = 0;
		EXPECT_FALSE(ValidateToolDescriptor(Descriptor));
		Descriptor = MakeDescriptor();
		Descriptor.Effects = static_cast<EToolEffect>(128);
		EXPECT_FALSE(ValidateToolDescriptor(Descriptor));
		Descriptor = MakeDescriptor();
		Descriptor.Effects = EToolEffect::MutatesMemory | EToolEffect::WritesStorage;
		EXPECT_TRUE(ValidateToolDescriptor(Descriptor));
	}

	TEST(FToolTests, KeepsSchemaOutputAndBudgetErrorsDistinct)
	{
		auto Descriptor = MakeDescriptor();
		Descriptor.Input = FToolSchema(EToolValueType::Object);
		Descriptor.Output = FToolSchema(EToolValueType::Number);
		Descriptor.Output.Minimum = std::numeric_limits<double>::infinity();
		EXPECT_EQ(EToolError::InvalidSchema, ValidateToolDescriptor(Descriptor).error().Code);
		Descriptor.Output.Minimum = 0.0;
		FToolLimits Limits;
		Limits.MaxNodes = 2; // Input and output nodes fit; the bound value does not.
		EXPECT_EQ(EToolError::LimitExceeded, ValidateToolDescriptor(Descriptor, Limits).error().Code);
		Descriptor.Output.Minimum.reset();
		Descriptor.Output.Enum = {std::numeric_limits<double>::quiet_NaN()};
		EXPECT_EQ(EToolError::InvalidSchema, ValidateToolDescriptor(Descriptor).error().Code);
		Descriptor.Output.Enum.clear();
		Descriptor.Output.Default = std::numeric_limits<double>::infinity();
		EXPECT_EQ(EToolError::InvalidSchema, ValidateToolDescriptor(Descriptor).error().Code);
		Descriptor.Output.Default.reset();
		EXPECT_EQ(EToolError::InvalidOutput, ValidateToolOutput(Descriptor, std::numeric_limits<double>::infinity()).error().Code);
		Descriptor.Output = FToolSchema(EToolValueType::Array);
		Descriptor.Output.Items = std::make_unique<FToolSchema>(EToolValueType::String);
		Descriptor.Output.MinSize = 1;
		Descriptor.Output.MaxSize = 2;
		EXPECT_FALSE(ValidateToolOutput(Descriptor, FToolValue::FArray{}));
		EXPECT_FALSE(ValidateToolOutput(Descriptor, FToolValue::FArray{"a", "b", "c"}));
		EXPECT_TRUE(ValidateToolOutput(Descriptor, FToolValue::FArray{"a", "b"}));
	}

	TEST(FToolTests, BoundsSchemaRawInputAndNormalizedDefaultExpansion)
	{
		auto Descriptor = MakeDescriptor();
		FToolLimits Limits;
		Limits.MaxDepth = 0;
		EXPECT_EQ(EToolError::LimitExceeded, ValidateToolDescriptor(Descriptor, Limits).error().Code);
		Limits = {};
		Limits.MaxStringBytes = 3;
		EXPECT_EQ(EToolError::LimitExceeded, ValidateToolDescriptor(Descriptor, Limits).error().Code);
		Limits = {};
		Limits.MaxTotalStringBytes = 10;
		EXPECT_EQ(EToolError::LimitExceeded, ValidateToolDescriptor(Descriptor, Limits).error().Code);
		Limits = {};
		Limits.MaxNodes = 1;
		EXPECT_EQ(EToolError::LimitExceeded, ValidateToolDescriptor(Descriptor, Limits).error().Code);
		Limits = {};
		Limits.MaxDepth = 65;
		EXPECT_EQ(EToolError::InvalidSchema, ValidateToolDescriptor(Descriptor, Limits).error().Code);
		Limits = {};
		Limits.MaxContainerEntries = 1;
		EXPECT_EQ(EToolError::LimitExceeded, PrepareToolArguments(Descriptor, Request({{"enabled", true}, {"extra", true}}), Limits).error().Code);

		// A repeated default fits in metadata but can exceed the normalized byte budget.
		Descriptor = MakeDescriptor();
		FToolSchema Entries(EToolValueType::Array);
		Entries.Items = std::make_unique<FToolSchema>(EToolValueType::Object);
		FToolSchema Label(EToolValueType::String);
		Label.Default = "0123456789";
		Entries.Items->Properties.emplace("label", Label);
		Descriptor.Input.Properties.emplace("entries", std::move(Entries));
		Limits = {};
		Limits.MaxTotalStringBytes = 128;
		ASSERT_TRUE(ValidateToolDescriptor(Descriptor, Limits));
		FToolValue::FArray Repeated(20, FToolValue::FObject{});
		EXPECT_EQ(EToolError::LimitExceeded, PrepareToolArguments(Descriptor, Request({{"entries", Repeated}}), Limits).error().Code);

		Descriptor = MakeDescriptor();
		FToolSchema Nested(EToolValueType::Object);
		Nested.Properties.emplace("child", FToolSchema(EToolValueType::Object));
		Nested.Default = FToolValue::FObject{{"child", FToolValue::FObject{}}};
		Descriptor.Input.Properties.emplace("nested", Nested);
		Limits = {};
		Limits.MaxDepth = 3;
		EXPECT_TRUE(ValidateToolDescriptor(Descriptor, Limits));
		EXPECT_TRUE(PrepareToolArguments(Descriptor, Request(), Limits));
		// An unrecognized input subtree is still bounded before an error path is constructed.
		FToolValue Deep = FToolValue::FObject{};
		for (int Index = 0; Index < 5; ++Index) Deep = FToolValue::FArray{std::move(Deep)};
		EXPECT_EQ(EToolError::LimitExceeded, PrepareToolArguments(Descriptor, Request({{"extra", Deep}}), Limits).error().Code);
	}

	TEST(FToolTests, ProviderSnapshotsAndExecutionRemainOwnedAfterUnregistration)
	{

		std::vector<FToolDescriptor> Descriptions;
		FToolResult Result;
		auto& Registry = FModularFeatureRegistry::Get();
		uint64 Identity = 0;
		{
			FTestToolset Provider;
			auto Registration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Provider);
			const auto Discovery = Registry.InvokeSingle<IToolset>([](auto& Value) { return Value.DescribeToolset(); });
			ASSERT_TRUE(Discovery.WasInvoked());
			Identity = Discovery.RegistrationIdentity;
			Descriptions = Discovery.Value->Tools;
			const auto Prepared = PrepareToolArguments(Descriptions.front(), Request());
			ASSERT_TRUE(Prepared.has_value());
			const auto Invocation = Registry.InvokeRegistered<IToolset>(Identity, [&](auto& Value) {
				FToolContext Context{"invocation", "session"};
				EXPECT_TRUE(Value.CheckAvailability(*Prepared, Context).bAvailable);
				return Value.Call(*Prepared, Context);
			});
			ASSERT_TRUE(Invocation.WasInvoked());
			Result = *Invocation.Value;
			Registration.Reset();
		}
		EXPECT_EQ(EToolStatus::Succeeded, Result.Status);
		EXPECT_TRUE(ValidateToolOutput(Descriptions.front(), Result.Data));
		EXPECT_EQ(EFeatureInvokeStatus::Unavailable, Registry.InvokeRegistered<IToolset>(Identity, [](auto&) {}).Status);
	}

	TEST(FToolTests, CancellationTokensAreReadOnlySharedAndSafeAcrossThreads)
	{
		FToolCancellationSource Source;
		const auto Token = Source.GetToken();
		EXPECT_FALSE(Token.IsCancellationRequested());
		EXPECT_FALSE(FToolCancellationToken{}.IsCancellationRequested());
		std::thread Cancel([&] { Source.Cancel(); });
		Cancel.join();
		EXPECT_TRUE(Token.IsCancellationRequested());
		Source.Cancel();
		EXPECT_TRUE(Source.GetToken().IsCancellationRequested());
	}

	TEST(FToolTests, PreflightsDetachedValuesBeforeSchemaOrCrossThreadCopy)
	{
		EXPECT_TRUE(ValidateToolValue(FToolValue::FObject{{"nested", FToolValue::FArray{int64{1}, true}}}));
		FToolLimits Limits; Limits.MaxDepth = 0;
		EXPECT_EQ(EToolError::LimitExceeded, ValidateToolValue(FToolValue::FArray{1}, Limits).error().Code);
		EXPECT_FALSE(ValidateToolValue(std::numeric_limits<double>::infinity()));
		EXPECT_FALSE(ValidateToolValue(std::string(65537, 'x')));
	}
}

namespace Durin::Tests
{
	namespace
	{
		int TypedCalls = 0;
		auto TypedAdd(const FToolContext& Context, int8 Left, uint64 Right) -> uint64
		{
			++TypedCalls;
			EXPECT_EQ("typed", Context.InvocationId);
			return static_cast<uint64>(Left) + Right;
		}
		auto TypedValues(const FToolContext&, bool Enabled, int32 Count, double Scale,
			const std::string& Label, const std::vector<int32>& Values) -> std::vector<std::string>
		{
			EXPECT_TRUE(Enabled); EXPECT_EQ(2, Count); EXPECT_EQ(1.5, Scale);
			EXPECT_EQ((std::vector<int32>{3, 4}), Values);
			return {Label, "done"};
		}
		auto TypedVoid(const FToolContext&) -> void { ++TypedCalls; }
	}

	TEST(FToolTests, GeneratesTypedSchemasDefaultsAndExactIntegerConversionsWithoutReflection)
	{
		TypedCalls = 0;
		auto Binding = BindTool(FToolDescriptor{.Name = "add"}, &TypedAdd,
			std::array{FToolParameter{"left"}, FToolParameter{"right", "Exact integer", FToolValue{uint64{9007199254740993ull}}}});
		const auto& Left = Binding.Descriptor.Input.Properties.at("left");
		EXPECT_EQ(EToolValueType::Integer, Left.Type);
		EXPECT_EQ(FToolValue(int8{-128}), Left.Minimum);
		EXPECT_EQ(FToolValue(int8{127}), Left.Maximum);
		EXPECT_EQ(EToolValueType::UnsignedInteger, Binding.Descriptor.Output.Type);
		FNativeToolset Set("math", "Arithmetic tools", {std::move(Binding)});
		FToolRequest Input{{"math", "add"}, 1, FToolValue::FObject{{"left", 1}}};
		auto Prepared = PrepareToolArguments(Set.DescribeToolset().Tools.front(), Input); ASSERT_TRUE(Prepared);
		auto Result = Set.Call(*Prepared, FToolContext{"typed"});
		ASSERT_EQ(EToolStatus::Succeeded, Result.Status);
		EXPECT_EQ(FToolValue(uint64{9007199254740994ull}), Result.Data);
		EXPECT_EQ(1, TypedCalls);
		Input.Arguments = FToolValue::FObject{{"left", 128}};
		EXPECT_EQ(EToolError::InvalidArguments, PrepareToolArguments(Set.DescribeToolset().Tools.front(), Input).error().Code);
		EXPECT_EQ(1, TypedCalls);
		Input.Address.Toolset = "other";
		EXPECT_FALSE(Set.CheckAvailability(Input, {}).bAvailable);
		EXPECT_EQ(EToolError::ToolUnavailable, Set.Call(Input, {}).Diagnostics.front().Code);
	}

	TEST(FToolTests, BindsOwnedStringsArraysScalarsAndConstReferences)
	{
		auto Binding = BindTool(FToolDescriptor{.Name = "values"}, &TypedValues,
			std::array{FToolParameter{"enabled"}, FToolParameter{"count"}, FToolParameter{"scale"},
				FToolParameter{"label"}, FToolParameter{"values"}});
		FNativeToolset Set("test", "", {std::move(Binding)});
		FToolRequest Input{{"test", "values"}, 1, FToolValue::FObject{
			{"enabled", true}, {"count", 2}, {"scale", 1.5}, {"label", "label"}, {"values", FToolValue::FArray{3, 4}}}};
		auto Prepared = PrepareToolArguments(Set.DescribeToolset().Tools.front(), Input); ASSERT_TRUE(Prepared);
		auto Result = Set.Call(*Prepared, {});
		ASSERT_EQ(EToolStatus::Succeeded, Result.Status);
		EXPECT_EQ((FToolValue(FToolValue::FArray{"label", "done"})), Result.Data);
		EXPECT_TRUE(ValidateToolOutput(Set.DescribeToolset().Tools.front(), Result.Data));
	}

	TEST(FToolTests, BindsVoidFunctionsAndNoArguments)
	{
		TypedCalls = 0;
		auto Binding = BindTool(FToolDescriptor{.Name = "touch"}, &TypedVoid, std::array<FToolParameter, 0>{});
		EXPECT_EQ(EToolValueType::Null, Binding.Descriptor.Output.Type);
		FNativeToolset Set("test", "", {std::move(Binding)});
		auto Prepared = PrepareToolArguments(Set.DescribeToolset().Tools.front(), {{"test", "touch"}, 1}); ASSERT_TRUE(Prepared);
		auto Result = Set.Call(*Prepared, {});
		EXPECT_EQ(EToolStatus::Succeeded, Result.Status);
		EXPECT_EQ(FToolValue{}, Result.Data);
		EXPECT_EQ(1, TypedCalls);
	}

	TEST(FToolTests, RejectsInvalidNativeBindingsBeforeRegistration)
	{
		auto Make = [] { return BindTool(FToolDescriptor{.Name = "touch"}, &TypedVoid, std::array<FToolParameter, 0>{}); };
		EXPECT_THROW((FNativeToolset("test", "", {Make(), Make()})), std::invalid_argument);
		EXPECT_THROW((FNativeToolset("test", "", {FToolBinding{FToolDescriptor{.Name = "missing"}}})), std::invalid_argument);
		EXPECT_THROW((FNativeToolset("Invalid", "", {})), std::invalid_argument);
		EXPECT_THROW((BindTool(FToolDescriptor{.Name = "add"}, &TypedAdd,
			std::array{FToolParameter{"same"}, FToolParameter{"same"}})), std::invalid_argument);
		EXPECT_THROW((BindTool(FToolDescriptor{.Name = "add"}, &TypedAdd,
			std::array{FToolParameter{"left", "", FToolValue{128}}, FToolParameter{"right"}})), std::invalid_argument);
	}

	TEST(FToolTests, ValidatesToolsetMetadataAndIndependentAddressParts)
	{
		EXPECT_TRUE(ValidateToolsetDescriptor({"editor.pie", "Play controls", {}}));
		EXPECT_TRUE(ValidateToolName("start"));
		EXPECT_TRUE(ValidateToolName("set_paused"));
		EXPECT_TRUE(ValidateToolsetName("editor.pie"));
		EXPECT_FALSE(ValidateToolName("pie.play"));
		EXPECT_FALSE(ValidateToolsetName("editor..pie"));
		EXPECT_FALSE(ValidateToolsetName("editor."));
		EXPECT_FALSE(ValidateToolName("bad/name"));
		EXPECT_FALSE(ValidateToolName(std::string(257, 'a')));
		auto Dotted = MakeDescriptor(); Dotted.Name = "pie.play";
		EXPECT_EQ(EToolError::InvalidDescriptor, ValidateToolDescriptor(Dotted).error().Code);
		EXPECT_THROW((BindTool(FToolDescriptor{.Name = "pie.play"}, &TypedVoid,
			std::array<FToolParameter, 0>{})), std::invalid_argument);
		FToolLimits Limits; Limits.MaxStringBytes = 4;
		EXPECT_EQ(EToolError::LimitExceeded, ValidateToolsetDescriptor({"test", "oversized", {}}, Limits).error().Code);
		auto Input = Request(); Input.Address.Toolset.clear();
		EXPECT_EQ(EToolError::InvalidArguments, PrepareToolArguments(MakeDescriptor(), Input).error().Code);
	}

	TEST(FToolTests, NativeBindingsUseThePreparedRequestWithoutCopyingOrNormalizingAgain)
	{
		FToolBinding Binding;
		Binding.Descriptor = MakeDescriptor();
		const FToolRequest* Seen = nullptr;
		Binding.Handler = [&](const FToolRequest& Request, const FToolContext&) {
			Seen = &Request;
			return FToolResult{EToolStatus::Succeeded, Request.Arguments};
		};
		FNativeToolset Set("tests", "", {std::move(Binding)});
		auto Prepared = PrepareToolArguments(Set.DescribeToolset().Tools.front(), Request());
		ASSERT_TRUE(Prepared);
		EXPECT_EQ(EToolStatus::Succeeded, Set.Call(*Prepared, {}).Status);
		EXPECT_EQ(&*Prepared, Seen);
		EXPECT_TRUE(std::get<bool>(std::get<FToolValue::FObject>(Seen->Arguments.Data).at("enabled").Data));
		Prepared->ContractVersion = 2;
		EXPECT_EQ(EToolError::VersionMismatch, Set.Call(*Prepared, {}).Diagnostics.front().Code);
	}
}
