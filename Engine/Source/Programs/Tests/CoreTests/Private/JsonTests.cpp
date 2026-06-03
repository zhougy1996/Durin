#include "Json/Json.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeJsonTestPath(std::string_view FileName) -> std::filesystem::path
	{
		return std::filesystem::current_path() / std::string(FileName);
	}

	TEST(FJsonDocumentTests, ParseObjectFromString)
	{
		Durin::FJsonDocument Document;
		Durin::FJsonParseError Error;

		ASSERT_TRUE(Document.Parse(R"({
			"name": "yyjson smoke test",
			"version": 1,
			"flags": {
				"enabled": true,
				"threshold": 0.75
			}
		})", &Error));
		EXPECT_EQ(Error.Code, 0);

		const Durin::FJsonNodeView Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsObject());
		EXPECT_TRUE(Root.Contains("flags"));
		EXPECT_EQ(Root.GetStringValue("name"), "yyjson smoke test");
		EXPECT_EQ(Root.GetIntValue("version"), 1);

		const Durin::FJsonNodeView Flags = Root.GetView("flags");
		ASSERT_TRUE(Flags.IsObject());
		EXPECT_TRUE(Flags.GetBoolValue("enabled"));
		EXPECT_DOUBLE_EQ(Flags.GetDoubleValue("threshold"), 0.75);
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
		Durin::FJsonParseError Error;

		ASSERT_TRUE(Document.LoadFromFile(CORE_TEST_SAMPLE_JSON_FILE, &Error));
		EXPECT_EQ(Error.Code, 0);

		const Durin::FJsonNodeView Root = Document.GetRootView();
		ASSERT_TRUE(Root.IsObject());
		EXPECT_EQ(Root.GetStringValue("name"), "yyjson smoke test");

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
		Root.SetStringValue("name", "writer");
		Root.SetBoolValue("enabled", true);
		Root.SetDoubleValue("threshold", 1.5);
		Root.SetNullValue("optional");

		Durin::FJsonNodeRef Flags = Root.AddObject("flags");
		Flags.SetUIntValue("count", 3);

		Durin::FJsonNodeRef Features = Root.AddArray("features");
		Features.AppendString("parse");
		Features.AppendInt(12);
		Features.AppendBool(false);
		Features.AppendNull();

		Durin::FJsonNodeRef NestedObject = Features.AppendObject();
		NestedObject.SetStringValue("label", "nested");

		Durin::FJsonNodeRef NestedArray = Features.AppendArray();
		NestedArray.AppendUInt(7);
		NestedArray.AppendString("tail");

		EXPECT_EQ(Root.GetRef("flags").GetUIntValue("count"), 3U);
		EXPECT_TRUE(Root.GetView("optional").IsNull());
		EXPECT_TRUE(Features.GetView(3).IsNull());
		EXPECT_EQ(Features.GetRef(4).GetStringValue("label"), "nested");
		EXPECT_EQ(Features.GetRef(5).GetView(1).GetString(), "tail");

		const std::filesystem::path OutputPath = MakeJsonTestPath("JsonRoundTripObject.json");
		ASSERT_TRUE(Document.SaveToFile(OutputPath.string()));

		Durin::FJsonDocument ReloadedDocument;
		Durin::FJsonParseError Error;
		ASSERT_TRUE(ReloadedDocument.LoadFromFile(OutputPath.string(), &Error));
		EXPECT_EQ(Error.Code, 0);

		const Durin::FJsonNodeView ReloadedRoot = ReloadedDocument.GetRootView();
		ASSERT_TRUE(ReloadedRoot.IsObject());
		EXPECT_EQ(ReloadedRoot.GetStringValue("name"), "writer");
		EXPECT_TRUE(ReloadedRoot.GetBoolValue("enabled"));
		EXPECT_DOUBLE_EQ(ReloadedRoot.GetDoubleValue("threshold"), 1.5);
		EXPECT_TRUE(ReloadedRoot.GetView("optional").IsNull());
		EXPECT_EQ(ReloadedRoot.GetView("flags").GetUIntValue("count"), 3U);

		const Durin::FJsonNodeView ReloadedFeatures = ReloadedRoot.GetView("features");
		ASSERT_TRUE(ReloadedFeatures.IsArray());
		ASSERT_EQ(ReloadedFeatures.Num(), 6U);
		EXPECT_EQ(ReloadedFeatures.GetView(0).GetString(), "parse");
		EXPECT_EQ(ReloadedFeatures.GetView(1).GetInt(), 12);
		EXPECT_FALSE(ReloadedFeatures.GetView(2).GetBool(true));
		EXPECT_TRUE(ReloadedFeatures.GetView(3).IsNull());
		EXPECT_EQ(ReloadedFeatures.GetView(4).GetStringValue("label"), "nested");
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
			Root.AppendString("array-root");
			Root.AppendNull();
			Durin::FJsonNodeRef ObjectValue = Root.AppendObject();
			ObjectValue.SetBoolValue("enabled", true);

			const std::filesystem::path OutputPath = MakeJsonTestPath("JsonArrayRoot.json");
			ASSERT_TRUE(Document.SaveToFile(OutputPath.string()));

			Durin::FJsonDocument ReloadedDocument;
			ASSERT_TRUE(ReloadedDocument.LoadFromFile(OutputPath.string()));
			const Durin::FJsonNodeView ReloadedRoot = ReloadedDocument.GetRootView();
			ASSERT_TRUE(ReloadedRoot.IsArray());
			ASSERT_EQ(ReloadedRoot.Num(), 3U);
			EXPECT_EQ(ReloadedRoot.GetView(0).GetString(), "array-root");
			EXPECT_TRUE(ReloadedRoot.GetView(1).IsNull());
			EXPECT_TRUE(ReloadedRoot.GetView(2).GetBoolValue("enabled"));

			std::error_code ErrorCode;
			std::filesystem::remove(OutputPath, ErrorCode);
		}

		{
			Durin::FJsonDocument Document;
			Durin::FJsonNodeRef Root = Document.GetMutableRoot();
			Root.SetString("scalar-root");

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
		Root.SetStringValue("name", "after");
		Root.SetNullValue("optional");

		Durin::FJsonNodeRef Items = Root.GetRef("items");
		ASSERT_TRUE(Items.IsArray());
		Items.AppendInt(3);

		const std::string JsonText = Document.ToString();
		EXPECT_FALSE(JsonText.empty());

		Durin::FJsonDocument ReloadedDocument;
		ASSERT_TRUE(ReloadedDocument.Parse(JsonText));
		const Durin::FJsonNodeView ReloadedRoot = ReloadedDocument.GetRootView();
		EXPECT_EQ(ReloadedRoot.GetStringValue("name"), "after");
		EXPECT_TRUE(ReloadedRoot.GetView("optional").IsNull());
		ASSERT_TRUE(ReloadedRoot.GetView("items").IsArray());
		EXPECT_EQ(ReloadedRoot.GetView("items").GetView(2).GetInt(), 3);
	}

	TEST(FJsonDocumentTests, InvalidJsonReportsErrorLocation)
	{
		Durin::FJsonDocument Document;
		Durin::FJsonParseError Error;

		EXPECT_FALSE(Document.Parse("{\"broken\": [1, 2, }", &Error));
		EXPECT_NE(Error.Code, 0);
		EXPECT_FALSE(Error.Message.empty());
		EXPECT_GT(Error.BytePosition, 0U);
		EXPECT_GT(Error.Line, 0U);
		EXPECT_GT(Error.Column, 0U);
	}
}
