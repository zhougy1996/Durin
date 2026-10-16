#include "Json/Json.h"
#include "Misc/FileHelper.h"
#include "NativeTestSupport.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeJsonTestPath(std::string_view FileName) -> std::filesystem::path
	{
		return Durin::Testing::GetTestWorkDirectory() / std::string(FileName);
	}

	auto MakeJsonTestDataPath(std::string_view FileName) -> std::filesystem::path
	{
		return std::filesystem::path{DURIN_TEST_DATA_DIR} / std::string(FileName);
	}

	TEST(FJsonDocumentTests, MissingFilePreservesIoDiagnostic)
	{
		Durin::FJsonDocument Document;

		const auto Path = MakeJsonTestPath("MissingFileDiagnostic.json");
		ASSERT_FALSE(std::filesystem::exists(Path));
		const auto Loaded = Document.LoadFromFile(Path);
		ASSERT_FALSE(Loaded);
		ASSERT_TRUE(std::holds_alternative<Durin::FFileError>(Loaded.error().Cause));
		EXPECT_EQ(std::get<Durin::FFileError>(Loaded.error().Cause).NativeError, std::errc::no_such_file_or_directory);
		EXPECT_NE(Loaded.error().ToString().find("open for reading"), std::string::npos);
		EXPECT_NE(Loaded.error().ToString().find(Path.filename().string()), std::string::npos);
	}

	TEST(FJsonDocumentTests, LoadDistinguishesParseFailureAndPreservesDocumentOnReadFailure)
	{
		Durin::FJsonDocument Document;
		ASSERT_TRUE(Document.Parse("{\"value\": 17}"));
		const auto Path = MakeJsonTestPath("MalformedExpected.json");
		const auto MissingPath = Path.string() + ".missing";
		ASSERT_FALSE(std::filesystem::exists(MissingPath));
		ASSERT_FALSE(Document.LoadFromFile(MissingPath));
		EXPECT_EQ(Document.GetRootView().GetView("value").GetInt(), 17);
		const std::string_view Text = "{\n\"broken\": [1, 2, }";
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text)), Path));
		const auto Loaded = Document.LoadFromFile(Path);
		ASSERT_FALSE(Loaded);
		ASSERT_TRUE(std::holds_alternative<Durin::FJsonParseError>(Loaded.error().Cause));
		const auto& Error = std::get<Durin::FJsonParseError>(Loaded.error().Cause);
		EXPECT_NE(Error.Code, 0);
		EXPECT_GT(Error.Line, 0u);
		EXPECT_FALSE(Document.IsValid());
		EXPECT_NE(Loaded.error().ToString().find("line"), std::string::npos);
		ASSERT_TRUE(Document.Parse("{}"));
		EXPECT_TRUE(Document.IsValid());
	}

	TEST(FJsonDocumentTests, ParseObjectFromString)
	{
		Durin::FJsonDocument Document;

		ASSERT_TRUE(Document.Parse(R"({
			"name": "yyjson smoke test",
			"version": 1,
			"flags": {
				"enabled": true,
				"threshold": 0.75
			}
		})"));

		const Durin::FJsonNodeView Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsObject());
		EXPECT_TRUE(Root.Contains("flags"));
		std::string Name;
		int64 Version = 0;
		EXPECT_TRUE(Root.GetChildValue("name", Name));
		EXPECT_TRUE(Root.GetChildValue("version", Version));
		EXPECT_EQ(Name, "yyjson smoke test");
		EXPECT_EQ(Version, 1);

		const Durin::FJsonNodeView Flags = Root.GetView("flags");
		ASSERT_TRUE(Flags.IsObject());
		EXPECT_TRUE(Flags.GetView("enabled").GetBool());
		EXPECT_DOUBLE_EQ(Flags.GetView("threshold").GetDouble(), 0.75);
	}

	TEST(FJsonDocumentTests, SelectsCompactOutputWithoutChangingTheDefaultFormat)
	{
		Durin::FJsonDocument Document;
		ASSERT_TRUE(Document.Parse(R"({ "label": "keep these spaces", "items": [true, null, {"value": 42}] })"));
		const auto Pretty = Document.ToString();
		EXPECT_EQ(Pretty, Document.ToString(Durin::EJsonFormat::Pretty));
		EXPECT_NE(std::string::npos, Pretty.find('\n'));
		EXPECT_EQ(R"({"label":"keep these spaces","items":[true,null,{"value":42}]})",
			Document.ToString(Durin::EJsonFormat::Compact));
		Document.GetMutableRoot().SetChildValue("label", "edited value");
		EXPECT_EQ(R"({"label":"edited value","items":[true,null,{"value":42}]})",
			Document.ToString(Durin::EJsonFormat::Compact));
	}

	TEST(FJsonDocumentTests, CompactOutputPreservesEscapesUnicodeAndNumericBoundaries)
	{
		Durin::FJsonDocument Document;
		auto Root = Document.GetMutableRoot();
		Root.EnsureObject();
		const std::string Text = " leading space\t\n\r\"\\ " "\xe4\xb8\xad\xf0\x9f\x98\x80" " trailing space ";
		Root.SetChildValue("spaced key", Text);
		auto Values = Root.AddArray("values");
		Values.AppendValue(UINT64_MAX);
		Values.AppendValue(INT64_MIN);
		Values.AppendValue(1.25);
		Values.AppendValue(nullptr);
		Values.AppendValue(false);
		const auto Compact = Document.ToString(Durin::EJsonFormat::Compact);
		EXPECT_EQ(std::string::npos, Compact.find_first_of("\r\n\t"));
		EXPECT_NE(std::string::npos, Compact.find("18446744073709551615"));
		EXPECT_NE(std::string::npos, Compact.find("-9223372036854775808"));
		EXPECT_TRUE(std::ranges::all_of(Compact, [](unsigned char C) { return C < 128; }));
		Durin::FJsonDocument Reloaded;
		ASSERT_TRUE(Reloaded.Parse(Compact));
		EXPECT_EQ(Text, Reloaded.GetRootView().GetView("spaced key").GetString());
		const auto Items = Reloaded.GetRootView().GetView("values");
		ASSERT_EQ(5u, Items.Num());
		EXPECT_EQ(UINT64_MAX, Items.GetView(0).GetUInt());
		EXPECT_EQ(INT64_MIN, Items.GetView(1).GetInt());
		EXPECT_DOUBLE_EQ(1.25, Items.GetView(2).GetDouble());
		EXPECT_TRUE(Items.GetView(3).IsNull());
		EXPECT_FALSE(Items.GetView(4).GetBool(true));
		for (const auto* Scalar : {"null", "true", "18446744073709551615", "-9223372036854775808", "1.25", R"(" a b ")"})
		{
			Durin::FJsonDocument ScalarDocument;
			ASSERT_TRUE(ScalarDocument.Parse(Scalar));
			EXPECT_EQ(Scalar, ScalarDocument.ToString(Durin::EJsonFormat::Compact));
		}
	}

	TEST(FJsonDocumentTests, ParseArrayAndDefaults)
	{
		Durin::FJsonDocument Document;
		ASSERT_TRUE(Document.Parse(R"(["parse", 12, false])"));

		const Durin::FJsonNodeView Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsArray());
		ASSERT_EQ(Root.Num(), 3U);

		EXPECT_EQ(Root.GetView(0).GetString(), "parse");
		EXPECT_EQ(Root.GetView(1).GetInt(), 12);
		EXPECT_FALSE(Root.GetView(2).GetBool(true));

		EXPECT_EQ(Root.GetView(10).GetString("missing"), "missing");
		EXPECT_EQ(Root.GetView(10).GetInt(42), 42);
	}

	TEST(FJsonDocumentTests, LoadFromFile)
	{
		Durin::FJsonDocument Document;

		ASSERT_TRUE(Document.LoadFromFile(MakeJsonTestDataPath("Sample.json").string()));

		const Durin::FJsonNodeView Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsObject());
		EXPECT_EQ(Root.GetView("name").GetString(), "yyjson smoke test");

		const Durin::FJsonNodeView Features = Root.GetView("features");
		ASSERT_TRUE(Features.IsArray());
		ASSERT_EQ(Features.Num(), 3U);
		EXPECT_EQ(Features.GetView(0).GetString(), "parse");
		EXPECT_EQ(Features.GetView(1).GetString(), "load-file");
		EXPECT_EQ(Features.GetView(2).GetString(), "errors");
	}

	TEST(FJsonDocumentTests, BuildModifyAndRoundTripObjectRoot)
	{
		Durin::FJsonDocument Document;
		Durin::FJsonNodeRef Root = Document.GetMutableRoot();
		Root.EnsureObject();
		Root.SetChildValue("name", "writer");
		Root.SetChildValue("enabled", true);
		Root.SetChildValue("threshold", 1.5);
		Root.SetChildValue("optional", nullptr);

		Durin::FJsonNodeRef Flags = Root.AddObject("flags");
		Flags.SetChildValue("count", 3U);

		Durin::FJsonNodeRef Features = Root.AddArray("features");
		Features.AppendValue("parse");
		Features.AppendValue(12);
		Features.AppendValue(false);
		Features.AppendValue(nullptr);

		Durin::FJsonNodeRef NestedObject = Features.AppendObject();
		NestedObject.SetChildValue("label", "nested");

		Durin::FJsonNodeRef NestedArray = Features.AppendArray();
		NestedArray.AppendValue(7U);
		NestedArray.AppendValue("tail");

		EXPECT_EQ(Root.GetRef("flags").GetView("count").GetUInt(), 3U);
		EXPECT_TRUE(Root.GetView("optional").IsNull());
		EXPECT_TRUE(Features.GetView(3).IsNull());
		EXPECT_EQ(Features.GetRef(4).GetView("label").GetString(), "nested");
		EXPECT_EQ(Features.GetRef(5).GetView(1).GetString(), "tail");

		const std::filesystem::path OutputPath = MakeJsonTestPath("JsonRoundTripObject.json");
		ASSERT_TRUE(Document.SaveToFile(OutputPath.string()));

		Durin::FJsonDocument ReloadedDocument;

		ASSERT_TRUE(ReloadedDocument.LoadFromFile(OutputPath.string()));

		const Durin::FJsonNodeView ReloadedRoot = ReloadedDocument.GetRootView();
		ASSERT_TRUE(ReloadedRoot.IsObject());
		EXPECT_EQ(ReloadedRoot.GetView("name").GetString(), "writer");
		EXPECT_TRUE(ReloadedRoot.GetView("enabled").GetBool());
		EXPECT_DOUBLE_EQ(ReloadedRoot.GetView("threshold").GetDouble(), 1.5);
		EXPECT_TRUE(ReloadedRoot.GetView("optional").IsNull());
		EXPECT_EQ(ReloadedRoot.GetView("flags").GetView("count").GetUInt(), 3U);

		const Durin::FJsonNodeView ReloadedFeatures = ReloadedRoot.GetView("features");
		ASSERT_TRUE(ReloadedFeatures.IsArray());
		ASSERT_EQ(ReloadedFeatures.Num(), 6U);
		EXPECT_EQ(ReloadedFeatures.GetView(0).GetString(), "parse");
		EXPECT_EQ(ReloadedFeatures.GetView(1).GetInt(), 12);
		EXPECT_FALSE(ReloadedFeatures.GetView(2).GetBool(true));
		EXPECT_TRUE(ReloadedFeatures.GetView(3).IsNull());
		EXPECT_EQ(ReloadedFeatures.GetView(4).GetView("label").GetString(), "nested");
		EXPECT_EQ(ReloadedFeatures.GetView(5).GetView(0).GetUInt(), 7U);
		EXPECT_EQ(ReloadedFeatures.GetView(5).GetView(1).GetString(), "tail");

		std::error_code ErrorCode;
		std::filesystem::remove(OutputPath, ErrorCode);
	}

	TEST(FJsonDocumentTests, SupportsArrayAndScalarRoots)
	{
		{
			Durin::FJsonDocument Document;
			Durin::FJsonNodeRef Root = Document.GetMutableRoot();
			Root.EnsureArray();
			Root.AppendValue("array-root");
			Root.AppendValue(nullptr);
			Durin::FJsonNodeRef ObjectValue = Root.AppendObject();
			ObjectValue.SetChildValue("enabled", true);

			const std::filesystem::path OutputPath = MakeJsonTestPath("JsonArrayRoot.json");
			ASSERT_TRUE(Document.SaveToFile(OutputPath.string()));

			Durin::FJsonDocument ReloadedDocument;
			ASSERT_TRUE(ReloadedDocument.LoadFromFile(OutputPath.string()));
			const Durin::FJsonNodeView ReloadedRoot = ReloadedDocument.GetRootView();
			ASSERT_TRUE(ReloadedRoot.IsArray());
			ASSERT_EQ(ReloadedRoot.Num(), 3U);
			EXPECT_EQ(ReloadedRoot.GetView(0).GetString(), "array-root");
			EXPECT_TRUE(ReloadedRoot.GetView(1).IsNull());
			EXPECT_TRUE(ReloadedRoot.GetView(2).GetView("enabled").GetBool());

			std::error_code ErrorCode;
			std::filesystem::remove(OutputPath, ErrorCode);
		}

		{
			Durin::FJsonDocument Document;
			Durin::FJsonNodeRef Root = Document.GetMutableRoot();
			Root.SetValue("scalar-root");

			const std::filesystem::path OutputPath = MakeJsonTestPath("JsonScalarRoot.json");
			ASSERT_TRUE(Document.SaveToFile(OutputPath.string()));

			Durin::FJsonDocument ReloadedDocument;
			ASSERT_TRUE(ReloadedDocument.LoadFromFile(OutputPath.string()));
			const Durin::FJsonNodeView ReloadedRoot = ReloadedDocument.GetRootView();
			ASSERT_TRUE(ReloadedRoot.IsString());
			EXPECT_EQ(ReloadedRoot.GetString(), "scalar-root");

			std::error_code ErrorCode;
			std::filesystem::remove(OutputPath, ErrorCode);
		}
	}

	TEST(FJsonDocumentTests, ParseThenMutateAndSerialize)
	{
		Durin::FJsonDocument Document;
		ASSERT_TRUE(Document.Parse(R"({
			"name": "before",
			"items": [1, 2]
		})"));

		Durin::FJsonNodeRef Root = Document.GetMutableRoot();
		Root.SetChildValue("name", "after");
		Root.SetChildValue("optional", nullptr);

		Durin::FJsonNodeRef Items = Root.GetRef("items");
		ASSERT_TRUE(Items.IsArray());
		Items.AppendValue(3);

		const std::string JsonText = Document.ToString();
		EXPECT_FALSE(JsonText.empty());

		Durin::FJsonDocument ReloadedDocument;
		ASSERT_TRUE(ReloadedDocument.Parse(JsonText));
		const Durin::FJsonNodeView ReloadedRoot = ReloadedDocument.GetRootView();
		EXPECT_EQ(ReloadedRoot.GetView("name").GetString(), "after");
		EXPECT_TRUE(ReloadedRoot.GetView("optional").IsNull());
		ASSERT_TRUE(ReloadedRoot.GetView("items").IsArray());
		EXPECT_EQ(ReloadedRoot.GetView("items").GetView(2).GetInt(), 3);
	}

	TEST(FJsonDocumentTests, InvalidJsonReportsErrorLocation)
	{
		Durin::FJsonDocument Document;

		const auto Parsed = Document.Parse("{\"broken\": [1, 2, }");
		ASSERT_FALSE(Parsed);
		const auto& Error = Parsed.error();
		EXPECT_NE(Error.Code, 0);
		EXPECT_FALSE(Error.Message.empty());
		EXPECT_GT(Error.BytePosition, 0U);
		EXPECT_GT(Error.Line, 0U);
		EXPECT_GT(Error.Column, 0U);
	}
}
