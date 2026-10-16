#include "MCPCodec.h"
#include <gtest/gtest.h>

namespace
{
	using namespace Durin;
	using namespace Durin::MCP;
	auto Request(std::string_view Arguments) -> std::string
	{
		return "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"arguments\":" + std::string(Arguments) + "}}";
	}
	TEST(FMCPCodecTests, PreservesIntegerBoundariesAndConvertsOnlyDeclaredNumericTypes)
	{
		auto Parsed = ParseRequest(Request("{\"signed\":9223372036854775807,\"unsigned\":18446744073709551615,\"negative\":-9223372036854775808,\"number\":5}"));
		ASSERT_TRUE(Parsed);
		FToolSchema Input(EToolValueType::Object);
		Input.Properties.emplace("signed", FToolSchema(EToolValueType::Integer));
		Input.Properties.emplace("unsigned", FToolSchema(EToolValueType::UnsignedInteger));
		Input.Properties.emplace("negative", FToolSchema(EToolValueType::Integer));
		Input.Properties.emplace("number", FToolSchema(EToolValueType::Number));
		auto Value = ConvertArguments(Find(Parsed->Params, "arguments"), Input);
		EXPECT_EQ(INT64_MAX, std::get<int64>(Find(Value, "signed").Data));
		EXPECT_EQ(UINT64_MAX, std::get<uint64>(Find(Value, "unsigned").Data));
		EXPECT_EQ(INT64_MIN, std::get<int64>(Find(Value, "negative").Data));
		EXPECT_EQ(5.0, std::get<double>(Find(Value, "number").Data));
		EXPECT_NE(std::string::npos, Serialize(Value).find("18446744073709551615"));
	}
	TEST(FMCPCodecTests, RejectsDuplicateKeysInvalidUtf8AndBoundedMalformedInput)
	{
		EXPECT_FALSE(ParseRequest(Request("{\"a\":1,\"a\":2}")));
		EXPECT_FALSE(ParseRequest(Request(std::string("\"") + char(0xff) + "\"")));
		EXPECT_FALSE(ParseRequest(std::string(MaxBodyBytes + 1, ' ')));
		EXPECT_FALSE(ParseRequest(Request(std::string(45, '[') + "0" + std::string(45, ']'))));
		EXPECT_FALSE(ParseRequest("[]"));
		EXPECT_FALSE(ParseRequest("{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":true,\"params\":{}}"));
		EXPECT_FALSE(ParseRequest(Request("NaN")));
		const auto LongString = Serialize(FObject{{"code", std::string(65536, 'x')}});
		const auto OversizedString = Serialize(FObject{{"code", std::string(128 * 1024 + 1, 'x')}});
		EXPECT_TRUE(ParseRequest(Request(LongString)));
		EXPECT_FALSE(ParseRequest(Request(OversizedString)));
	}
	TEST(FMCPCodecTests, SerializesSingleLineMessagesWithoutChangingStringContents)
	{
		const std::string Text = " keep spaces \t\r\n\"\\ " "\xe4\xb8\xad\xf0\x9f\x98\x80";
		auto Message = Reply("id", {{"text", Text}, {"nested", FArray{FObject{{"value", Text}}}}});
		const auto Wire = Serialize(Message);
		EXPECT_EQ(std::string::npos, Wire.find_first_of("\r\n\t"));
		EXPECT_TRUE(Wire.starts_with(R"({"id":"id","jsonrpc":"2.0","result":{)"));
		FJsonDocument Document;
		ASSERT_TRUE(Document.Parse(Wire));
		const auto Result = Document.GetRootView().GetView("result");
		EXPECT_EQ(Text, Result.GetView("text").GetString());
		EXPECT_EQ(Text, Result.GetView("nested").GetView(0).GetView("value").GetString());
	}
	TEST(FMCPCodecTests, DescribesEveryNativeSchemaKindAndUtf8ByteBounds)
	{
		for (auto Type : {EToolValueType::Null, EToolValueType::Boolean, EToolValueType::Integer,
			EToolValueType::UnsignedInteger, EToolValueType::Number, EToolValueType::String,
			EToolValueType::Array, EToolValueType::Object})
		{
			FToolSchema Native(Type);
			if (Type == EToolValueType::Array) Native.Items = std::make_unique<FToolSchema>(EToolValueType::Boolean);
			EXPECT_FALSE(Text(Find(Schema(Native), "type")).empty());
		}
		FToolSchema Native(EToolValueType::Object);
		FToolSchema Field(EToolValueType::String);
		Field.MinSize = 2; Field.MaxSize = 8; Field.Default = "ok"; Field.Enum = {"ok", "yes"};
		Native.Properties.emplace("text", Field); Native.Required = {"text"}; Native.MaxSize = 1;
		auto Wire = Schema(Native);
		const auto& TextSchema = Find(Find(Wire, "properties"), "text");
		EXPECT_EQ("ok", Text(Find(TextSchema, "default")));
		EXPECT_EQ(8u, std::get<uint64>(Find(TextSchema, "x-durin-maxUtf8Bytes").Data));
		EXPECT_TRUE(std::holds_alternative<std::monostate>(Find(TextSchema, "minLength").Data));
		EXPECT_FALSE(std::get<bool>(Find(Wire, "additionalProperties").Data));
	}
	TEST(FMCPCodecTests, FiltersPrivateToolsAndUsesStableBoundedNames)
	{
		FToolDescriptor Visible{.Name = "pause", .bExposeExternally = true};
		FToolDescriptor Hidden{.Name = "private"};
		FEditorToolCatalog Native;
		Native.Toolsets.push_back({{"editor.pie", "PIE", {Visible, Hidden}}, 1});
		auto Catalog = BuildCatalog(Native);
		ASSERT_TRUE(Catalog); ASSERT_EQ(1u, Catalog->size());
		EXPECT_TRUE(Catalog->contains("editor.pie.pause"));
		Native.Toolsets[0].Descriptor.Name = std::string(256, 'a');
		auto Long = BuildCatalog(Native);
		ASSERT_TRUE(Long); EXPECT_LE(Long->begin()->first.size(), 128u);
		EXPECT_EQ(Long->begin()->first, BuildCatalog(Native)->begin()->first);
		Native.Toolsets.push_back(Native.Toolsets[0]);
		EXPECT_FALSE(BuildCatalog(Native));
	}
	TEST(FMCPCodecTests, DescribesPythonJobRoutesAndSchemasExplicitly)
	{
		auto Catalog = BuildPythonCatalog(); ASSERT_TRUE(Catalog); ASSERT_EQ(3u, Catalog->size());
		EXPECT_EQ(EToolRoute::PythonSubmit, Catalog->at("editor.python.submit").Route);
		EXPECT_EQ(EToolRoute::PythonStatus, Catalog->at("editor.python.status").Route);
		EXPECT_EQ(EToolRoute::PythonCancel, Catalog->at("editor.python.cancel").Route);
		for (const auto& [Name, Tool] : *Catalog)
		{
			(void)Name;
			FToolDescriptor Descriptor; Descriptor.Name = Tool.Address.Tool; Descriptor.Input = Tool.Input;
			EXPECT_TRUE(ValidateToolDescriptor(Descriptor));
		}
		const auto& Submit = Catalog->at("editor.python.submit");
		EXPECT_EQ(65536u, Submit.Input.Properties.at("code").MaxSize);
		EXPECT_FALSE(std::get<bool>(Find(Find(Submit.Description, "annotations"), "idempotentHint").Data));
	}
	TEST(FMCPCodecTests, ListsTheCompleteCatalogInNameOrder)
	{
		FEditorToolCatalog Native;
		FToolsetDescriptor Group{"tests", "tests", {}};
		for (int I = 0; I < 20; ++I) Group.Tools.push_back({.Name = std::format("tool{}", I), .bExposeExternally = true});
		Native.Toolsets.push_back({Group, 1});
		auto Catalog = BuildCatalog(Native); ASSERT_TRUE(Catalog);
		auto Listed = List(1, *Catalog);
		const auto& Result = Find(Listed, "result");
		const auto& Tools = std::get<FArray>(Find(Result, "tools").Data);
		ASSERT_EQ(20u, Tools.size());
		for (size_t I = 1; I < Tools.size(); ++I) EXPECT_LT(Text(Find(Tools[I - 1], "name")), Text(Find(Tools[I], "name")));
		EXPECT_FALSE(std::get<FObject>(Result.Data).contains("nextCursor"));
		EXPECT_TRUE(std::get<FArray>(Find(Find(List(1, {}), "result"), "tools").Data).empty());
	}
	TEST(FMCPCodecTests, ClassifiesParseAndEnvelopeFailuresExplicitly)
	{
		for (const auto* Text : {"{", "{]", R"({"jsonrpc":"2.0","id":1,"method":"ping","params":NaN})"})
		{
			auto Result = ParseRequest(Text); ASSERT_FALSE(Result);
			EXPECT_EQ(ERequestErrorCode::ParseError, Result.error().Code);
		}
		for (const auto* Text : {"[]", R"({"jsonrpc":"2.0","id":true,"method":"ping"})",
			R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{"a":1,"a":2}})",
			R"({"jsonrpc":"2.0","id":1,"method":"ping","params":[]})"})
		{
			auto Result = ParseRequest(Text); ASSERT_FALSE(Result);
			EXPECT_EQ(ERequestErrorCode::InvalidRequest, Result.error().Code);
		}
	}
	TEST(FMCPCodecTests, JoinsDottedToolsetsWithoutEscapingUnderscores)
	{
		ASSERT_TRUE(ValidateToolName("a__b"));
		FEditorToolCatalog Native;
		Native.Toolsets.push_back({{"a__b", "first", {{.Name = "c", .bExposeExternally = true}}}, 1});
		Native.Toolsets.push_back({{"a", "second", {{.Name = "b__c", .bExposeExternally = true}}}, 2});
		Native.Toolsets.push_back({{"a_u", "third", {{.Name = "c", .bExposeExternally = true}}}, 3});
		Native.Toolsets.push_back({{"editor.pie", "fourth", {{.Name = "set_paused", .bExposeExternally = true}}}, 4});
		auto Catalog = BuildCatalog(Native); ASSERT_TRUE(Catalog); ASSERT_EQ(4u, Catalog->size());
		EXPECT_EQ("a__b", Catalog->at("a__b.c").Address.Toolset);
		EXPECT_EQ("b__c", Catalog->at("a.b__c").Address.Tool);
		EXPECT_EQ("a_u", Catalog->at("a_u.c").Address.Toolset);
		EXPECT_EQ("set_paused", Catalog->at("editor.pie.set_paused").Address.Tool);
	}
	TEST(FMCPCodecTests, RejectsDottedLocalNamesAndMalformedToolsets)
	{
		FEditorToolCatalog Native;
		Native.Toolsets.push_back({{"editor", "ambiguous", {{.Name = "pie.play", .bExposeExternally = true}}}, 1});
		EXPECT_FALSE(BuildCatalog(Native));
		Native.Toolsets[0].Descriptor.Name = "editor..pie";
		Native.Toolsets[0].Descriptor.Tools[0].Name = "play";
		EXPECT_FALSE(BuildCatalog(Native));
	}
	TEST(FMCPCodecTests, RejectsDuplicateNamesAndLongNameAliases)
	{
		FEditorToolCatalog Native;
		Native.Toolsets.push_back({{"editor.pie", "PIE", {{.Name = "play", .bExposeExternally = true}}}, 1});
		Native.Toolsets[0].Descriptor.Tools.push_back(Native.Toolsets[0].Descriptor.Tools[0]);
		auto Duplicate = BuildCatalog(Native); ASSERT_FALSE(Duplicate);
		EXPECT_NE(std::string::npos, Duplicate.error().find("editor.pie.play"));
		Native.Toolsets[0].Descriptor.Tools.pop_back();
		Native.Toolsets[0].Descriptor.Name = std::string(256, 'a');
		auto Long = BuildCatalog(Native); ASSERT_TRUE(Long);
		const auto Name = Long->begin()->first;
		ASSERT_EQ(128u, Name.size());
		const auto Separator = Name.rfind('.'); ASSERT_NE(std::string::npos, Separator);
		Native.Toolsets.push_back({{Name.substr(0, Separator), "alias",
			{{.Name = Name.substr(Separator + 1), .bExposeExternally = true}}}, 2});
		EXPECT_FALSE(BuildCatalog(Native));
		std::reverse(Native.Toolsets.begin(), Native.Toolsets.end());
		EXPECT_FALSE(BuildCatalog(Native));
	}
	TEST(FMCPCodecTests, EnvelopesScalarNullAndFailureResultsWithoutLeakingReceipts)
	{
		for (FToolValue Data : {FToolValue{}, FToolValue(true), FToolValue(FArray{1, 2}), FToolValue(FObject{{"state", "paused"}})})
		{
			auto Wire = ToolResult("id", {EToolStatus::Succeeded, Data});
			const auto& Result = Find(Wire, "result");
			EXPECT_EQ(Data, Find(Find(Result, "structuredContent"), "data"));
			EXPECT_TRUE(std::holds_alternative<std::monostate>(Find(Result, "resultType").Data));
		}
		auto Wire = ToolResult(1, {EToolStatus::Failed, {}, {{EToolError::Busy, "busy", "/field"}}});
		EXPECT_TRUE(std::get<bool>(Find(Find(Wire, "result"), "isError").Data));
		EXPECT_NE(std::string::npos, Serialize(Wire).find("/field"));
		EXPECT_NE(std::string::npos, Serialize(Initialize(1)).find("2025-06-18"));
	}
	TEST(FMCPCodecTests, ParsesSessionNotificationsWithoutInventingRequestIds)
	{
		auto Notification = ParseRequest(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
		ASSERT_TRUE(Notification); EXPECT_TRUE(Notification->bNotification);
		auto Cancel = ParseRequest(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":"a"}})");
		ASSERT_TRUE(Cancel); EXPECT_TRUE(Cancel->bNotification); EXPECT_EQ("a", Text(Find(Cancel->Params, "requestId")));
		EXPECT_FALSE(ParseRequest(R"({"jsonrpc":"2.0","id":null,"method":"ping"})"));
		auto Ping = ParseRequest(R"({"jsonrpc":"2.0","id":7,"method":"ping"})");
		ASSERT_TRUE(Ping); EXPECT_FALSE(Ping->bNotification);
	}
}
